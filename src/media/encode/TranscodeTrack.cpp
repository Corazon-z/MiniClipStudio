// S03由ExportMp4选择轨道并读包，这里负责把其中一条音轨或视频轨真正转成新的编码数据。
// PushPacket调用libavcodec还原源帧，ProcessDecoded把帧交给libavfilter裁剪/转换，ReceiveFiltered再交编码器。
// ReceiveEncoded取出H.264/AAC包后交Mp4Output保存；它们的时间单位也在这里换算，不能只把数据字节搬过去。
// 所有处理留在当前线程。Finish取完各级缓存，析构只释放资源，不能把“资源已释放”当成“文件已导出成功”。
#define NOMINMAX
#include "TranscodeTrack.h"
#include "../../export/TranscodeTiming.h"
#include <iomanip>
#include <locale>
#include <sstream>
extern "C" {
#include <libavutil/opt.h>
}

namespace miniclip {
namespace {
// OpenFilters拼接裁剪时间等参数时用它生成固定小数点的文字，避免系统区域设置把“.”改成逗号而使配方无法解析。
std::string Decimal(double value) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(9) << value;
    return text.str();
}
// OpenFilters调用libavutil的av_channel_layout_describe，把声道布局写成过滤器能接收的文字；后续也用它核对布局是否变化。
std::string Layout(const AVChannelLayout& value) {
    char text[256]{};
    Check(av_channel_layout_describe(&value, text, sizeof(text)), "Describe source layout");
    return text;
}
}
// ExportMp4为选中轨道创建本对象。先按源codec_id打开解码器，再为视频配置libx264、为音频配置AAC编码器，
// 最后在输出容器里建立对应AVStream并复制编码参数，供MP4描述数据。过滤器要等真实首帧到来后再建。
// 构造中途失败不会调用本类析构，因此catch显式Cleanup归还已分配资源；借来的输入、输出和指标不由本类销毁。
TranscodeTrack::TranscodeTrack(const AVStream* source, Mp4Output& output, const TranscodeOptions& options,
    int64_t clipStartUs, TranscodeMetrics& metrics, const std::atomic<bool>* cancel,
    std::function<void(const AVFrame*)> decodedVideo)
    : source_(source), output_(output), options_(options), metrics_(metrics), cancel_(cancel),
      decodedVideo_(std::move(decodedVideo)), video_(source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO), clipStartUs_(clipStartUs),
      decoded_(NewFrame()), filtered_(NewFrame()), encoded_(NewPacket()) {
    try {
        if (source->time_base.num <= 0 || source->time_base.den <= 0)
            throw std::runtime_error("Invalid source stream time base");
        const AVCodec* decoder = avcodec_find_decoder(source->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("Source decoder unavailable");
        decoder_ = avcodec_alloc_context3(decoder);
        if (!decoder_) throw std::bad_alloc();
        Check(avcodec_parameters_to_context(decoder_, source->codecpar), "Set transcode decoder parameters");
        decoder_->pkt_timebase = source->time_base;
        decoder_->max_pixels = 64000000;
        decoder_->err_recognition = AV_EF_EXPLODE;
        decoder_->thread_count = options.processingThreads;
        Check(avcodec_open2(decoder_, decoder, nullptr), "Open transcode decoder");
        // 视频目标为8位YUV420/BT.709的H.264；音频目标为AAC，编码器接收各声道分开存的浮点样本FLTP。
        const AVCodec* encoder = avcodec_find_encoder_by_name(video_ ? "libx264" : "aac");
        if (!encoder) throw std::runtime_error("Required libx264/AAC encoder unavailable");
        encoder_ = avcodec_alloc_context3(encoder);
        if (!encoder_) throw std::bad_alloc();
        encoder_->thread_count = options.processingThreads;
        if (video_) {
            encoder_->width = options.width ? options.width : decoder_->width;
            encoder_->height = options.height ? options.height : decoder_->height;
            if (encoder_->width <= 0 || encoder_->height <= 0 || encoder_->width > 8192 || encoder_->height > 8192
                || encoder_->width % 2 || encoder_->height % 2 || int64_t{encoder_->width} * encoder_->height > 33554432)
                throw std::runtime_error("Output requires even dimensions <=8192 and <=32M pixels");
            encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
            encoder_->time_base = AVRational{1, 90000};
            encoder_->framerate = AVRational{options.framesPerSecond, 1};
            encoder_->sample_aspect_ratio = AVRational{1, 1};
            encoder_->max_b_frames = 0;
            encoder_->gop_size = options.framesPerSecond * 2;
            encoder_->color_range = AVCOL_RANGE_MPEG;
            encoder_->colorspace = AVCOL_SPC_BT709;
            encoder_->color_primaries = AVCOL_PRI_BT709;
            encoder_->color_trc = AVCOL_TRC_BT709;
            Check(av_opt_set(encoder_->priv_data, "preset", "medium", 0), "Set x264 preset");
            Check(av_opt_set(encoder_->priv_data, "crf", "23", 0), "Set x264 quality");
            Check(av_opt_set(encoder_->priv_data, "tune", "zerolatency", 0), "Set x264 latency mode");
            metrics.width = encoder_->width;
            metrics.height = encoder_->height;
        } else {
            encoder_->sample_rate = options.sampleRate;
            encoder_->sample_fmt = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&encoder_->ch_layout, options.channels);
            encoder_->time_base = AVRational{1, options.sampleRate};
            encoder_->bit_rate = 128000;
            if (!(encoder->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME))
                throw std::runtime_error("AAC encoder cannot accept short final frame");
        }
        // MP4需要全局编码头时先通知编码器；open成功后，才把完整参数复制到容器新建的输出轨道。
        if (output.Context()->oformat->flags & AVFMT_GLOBALHEADER) encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        Check(avcodec_open2(encoder_, encoder, nullptr), "Open export encoder");
        destination_ = avformat_new_stream(output.Context(), nullptr);
        if (!destination_) throw std::bad_alloc();
        destination_->time_base = encoder_->time_base;
        if (video_) destination_->avg_frame_rate = encoder_->framerate;
        Check(avcodec_parameters_from_context(destination_->codecpar, encoder_), "Set MP4 stream parameters");
    } catch (...) { Cleanup(); throw; }
}
// 轨道离开作用域时只释放自己的资源；可能失败的尾部处理必须由调用方明确执行Finish，不能藏在析构里。
TranscodeTrack::~TranscodeTrack() { Cleanup(); }
// 构造失败或析构时调用它，分别用avfilter_graph_free和avcodec_free_context归还已取得的资源，空指针也可安全处理。
void TranscodeTrack::Cleanup() noexcept {
    avfilter_graph_free(&graph_);
    avcodec_free_context(&decoder_);
    avcodec_free_context(&encoder_);
}
// 解码、过滤、编码循环反复调用它，读取借用的原子停止标志，及时把用户取消传回外层ExportMp4。
void TranscodeTrack::CheckCancel() const {
    if (cancel_ && cancel_->load()) throw TranscodeCancelled();
}

// ProcessDecoded收到首帧后用它建立过滤器图，也就是按顺序连接的一组裁剪、时间和格式转换操作。
// 先用libavfilter创建视频buffer或音频abuffer输入与对应输出，再拼接配方，交graph_parse_ptr连接、graph_config验证。
// trim/atrim保留片段，setpts/asetpts减去两轨共用起点；视频再调帧率/尺寸，音频再重采样并按AAC输入要求分组。
// 图统一拥有输入输出节点，不能单独free；这里把配方交给FFmpeg C API解析，没有执行外部命令行。
void TranscodeTrack::OpenFilters(const AVFrame* first) {
    graph_ = avfilter_graph_alloc();
    if (!graph_) throw std::bad_alloc();
    graph_->nb_threads = options_.processingThreads;
    std::ostringstream args;
    args.imbue(std::locale::classic());
    args << "time_base=" << source_->time_base.num << '/' << source_->time_base.den;
    if (video_) {
        args << ":video_size=" << first->width << 'x' << first->height << ":pix_fmt=" << first->format
             << ":pixel_aspect=1/1";
        firstWidth_ = first->width;
        firstHeight_ = first->height;
    } else {
        const char* format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(first->format));
        if (!format) throw std::runtime_error("Unknown decoded audio sample format");
        firstRate_ = first->sample_rate;
        firstLayout_ = Layout(first->ch_layout);
        args << ":sample_rate=" << first->sample_rate << ":sample_fmt=" << format << ":channel_layout=" << firstLayout_;
    }
    firstFormat_ = first->format;
    const AVFilter* sourceFilter = avfilter_get_by_name(video_ ? "buffer" : "abuffer");
    const AVFilter* sinkFilter = avfilter_get_by_name(video_ ? "buffersink" : "abuffersink");
    if (!sourceFilter || !sinkFilter) throw std::runtime_error("Required buffer filters are unavailable");
    Check(avfilter_graph_create_filter(&filterSource_, sourceFilter,
        "source", args.str().c_str(), nullptr, graph_), "Create export filter source");
    Check(avfilter_graph_create_filter(&filterSink_, sinkFilter,
        "sink", nullptr, nullptr, graph_), "Create export filter sink");
    // 开始写配方：先指定保留的源时间范围，再从时间戳里减掉公共起点，让导出片段从自己的时间坐标开始。
    const double start = clipStartUs_ / 1000000.0;
    std::string trim = std::string(video_ ? "trim" : "atrim") + "=start=" + Decimal(start);
    if (options_.durationSeconds > 0) trim += ":end=" + Decimal(start + options_.durationSeconds);
    // 两轨都减同一个时间起点，不能各自减自己的首帧PTS而丢掉相对偏移。
    std::string chain = trim + (video_ ? ",setpts=PTS-" : ",asetpts=PTS-") + Decimal(start) + "/TB";
    if (video_) {
        chain += ",fps=fps=" + std::to_string(options_.framesPerSecond) + ":start_time=0:round=near:eof_action=pass";
        if (options_.durationSeconds > 0) chain += ",trim=duration=" + Decimal(options_.durationSeconds);
        chain += ",scale=" + std::to_string(encoder_->width) + ':' + std::to_string(encoder_->height)
            + ":out_color_matrix=bt709:out_range=tv,setsar=1,format=pix_fmts=yuv420p";
    } else {
        chain += ",aresample=" + std::to_string(options_.sampleRate) + ":async=1:first_pts=0,aformat=sample_fmts=fltp:sample_rates="
            + std::to_string(options_.sampleRate) + ":channel_layouts=" + (options_.channels == 2 ? "stereo" : "mono");
        if (options_.durationSeconds > 0) chain += ",atrim=duration=" + Decimal(options_.durationSeconds);
        // p=0保留最后不足一整AAC帧的真实样本数，不能用sink固定帧长偷偷补零冒充内容。
        chain += ",asetnsamples=n=" + std::to_string(encoder_->frame_size) + ":p=0";
    }
    // 临时InOut只描述配方接到哪个入口和出口，parse完成就释放；真正的过滤器节点仍归graph统一管理。
    AVFilterInOut* inputs = avfilter_inout_alloc();
    AVFilterInOut* outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) { avfilter_inout_free(&inputs); avfilter_inout_free(&outputs); throw std::bad_alloc(); }
    inputs->name = av_strdup("out"); inputs->filter_ctx = filterSink_; inputs->pad_idx = 0;
    outputs->name = av_strdup("in"); outputs->filter_ctx = filterSource_; outputs->pad_idx = 0;
    int status = (!inputs->name || !outputs->name) ? AVERROR(ENOMEM)
        : avfilter_graph_parse_ptr(graph_, chain.c_str(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    Check(status, "Connect export filters");
    Check(avfilter_graph_config(graph_, nullptr), "Configure export filters");
}

// ReceiveDecoded把每张解码帧交到这里。先检查PTS是否存在并递增，即每帧都有可信的播放时刻，再检查实际格式。
// 首帧调用OpenFilters建立转换路线；后续尺寸、样本格式或布局变了就明确报错，不继续按旧规格解释新数据。
// 校验通过后用buffersrc把帧交给过滤器，再ReceiveFiltered取走已经完成的结果。
void TranscodeTrack::ProcessDecoded(AVFrame* frame) {
    CheckCancel();
    if (frame->decode_error_flags) throw std::runtime_error("Decoder reported damaged source frame");
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) frame->pts = frame->best_effort_timestamp;
    if (frame->pts == AV_NOPTS_VALUE) throw std::runtime_error("Missing decoded source timestamp; cannot safely reconstruct timing");
    if (lastInputPts_ != AV_NOPTS_VALUE && frame->pts <= lastInputPts_)
        throw std::runtime_error("Non-increasing decoded source timestamp: " + std::to_string(frame->pts)
            + " after " + std::to_string(lastInputPts_));
    lastInputPts_ = frame->pts;
    // 画面目前只支持列出的软件8位SDR格式；音频只接受单双声道，未写明布局时补为对应默认布局。
    if (video_) {
        ++metrics_.decodedVideoFrames;
        switch (static_cast<AVPixelFormat>(frame->format)) {
        case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24: case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_NV12: case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: case AV_PIX_FMT_BGRA: case AV_PIX_FMT_BGR0: break;
        default: throw std::runtime_error("Only software 8-bit SDR pictures are supported");
        }
        if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67
            || frame->color_primaries == AVCOL_PRI_BT2020)
            throw std::runtime_error("HDR/wide-gamut conversion is not implemented");
        if (frame->width <= 0 || frame->height <= 0 || int64_t{frame->width} * frame->height > 64000000)
            throw std::runtime_error("Decoded picture exceeds source limits");
    } else {
        if (frame->sample_rate <= 0 || frame->sample_rate > 192000 || frame->nb_samples <= 0
            || frame->ch_layout.nb_channels < 1 || frame->ch_layout.nb_channels > 2)
            throw std::runtime_error("Only mono/stereo source audio is supported");
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&frame->ch_layout, frame->ch_layout.nb_channels);
    }
    // S06先将实际画面交给同步观察者复制BGRA；回调借用只在本次调用有效，不另建线程。
    if (video_ && decodedVideo_) decodedVideo_(frame);
    // 首帧建立过滤器输入，后续校验格式不变；KEEP_REF让图保留数据引用，调用方之后可unref源帧。
    if (!graph_) OpenFilters(frame);
    else if (firstFormat_ != frame->format || (video_ && (firstWidth_ != frame->width || firstHeight_ != frame->height))
        || (!video_ && (firstRate_ != frame->sample_rate || firstLayout_ != Layout(frame->ch_layout))))
        throw std::runtime_error("Dynamic source format changes are not supported");
    if (filterEnded_) return;
    int status;
    { ScopedSeconds time(metrics_.filterSeconds);
      status = av_buffersrc_add_frame_flags(filterSource_, frame, AV_BUFFERSRC_FLAG_KEEP_REF); }
    if (status == AVERROR_EOF) { filterEnded_ = true; return; }
    Check(status, "Feed export filters");
    ReceiveFiltered();
}
// PushPacket送包后、Finish通知结束后，都通过它循环调用avcodec_receive_frame取画面或声音样本。
// 每个结果先交ProcessDecoded继续处理，再解除帧引用；一个包不一定对应一帧，因此要取到EAGAIN或EOF为止。
int TranscodeTrack::ReceiveDecoded() {
    for (;;) {
        CheckCancel();
        int status;
        { ScopedSeconds time(metrics_.decodeSeconds); status = avcodec_receive_frame(decoder_, decoded_.get()); }
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return status;
        Check(status, "Receive decoded export frame");
        ProcessDecoded(decoded_.get());
        av_frame_unref(decoded_.get());
    }
}
// ExportMp4读到本轨数据后调用它，用avcodec_send_packet把源包送入解码器，再由ReceiveDecoded取帧。
// EAGAIN表示现在要先取结果，不能丢掉原包；先推进接收端再重送，并累计这些解码API经过的时间。
void TranscodeTrack::PushPacket(const AVPacket* packet) {
    CheckCancel();
    int status;
    { ScopedSeconds time(metrics_.decodeSeconds); status = avcodec_send_packet(decoder_, packet); }
    if (status == AVERROR(EAGAIN)) {
        ReceiveDecoded();
        ScopedSeconds time(metrics_.decodeSeconds); status = avcodec_send_packet(decoder_, packet);
    }
    Check(status, "Send export decoder packet");
    ReceiveDecoded();
}
// ProcessDecoded和Finish通过buffersink从过滤器取结果，然后把PTS换成编码器的时间单位，确定这一帧能覆盖多久。
// 接近片段终点时只保留真实有效的时长；送SendEncoded后记录视频帧数或音频样本数，而不是预留缓冲容量。
// 编码器可能延迟出包，因此用durations_记住每个PTS的有效时长，稍后ReceiveEncoded再给成品包补上。
int TranscodeTrack::ReceiveFiltered() {
    for (;;) {
        CheckCancel();
        int status;
        { ScopedSeconds time(metrics_.filterSeconds); status = av_buffersink_get_frame(filterSink_, filtered_.get()); }
        if (status == AVERROR_EOF) { filterEnded_ = true; return status; }
        if (status == AVERROR(EAGAIN)) return status;
        Check(status, "Receive filtered export frame");
        AVFrame* frame = filtered_.get();
        frame->pts = av_rescale_q(frame->pts, av_buffersink_get_time_base(filterSink_), encoder_->time_base);
        if (frame->pts == AV_NOPTS_VALUE || frame->pts < 0) throw std::runtime_error("Invalid filtered timestamp");
        // 有效时长按视频帧间隔或音频真实样本数计算，再截到所选片段终点，不能用缓冲容量冒充内容。
        int64_t duration = video_ ? av_rescale_q(1, AVRational{1, options_.framesPerSecond}, encoder_->time_base) : frame->nb_samples;
        if (options_.durationSeconds > 0) {
            const int64_t end = av_rescale_q(static_cast<int64_t>(std::llround(options_.durationSeconds * 1000000)),
                AVRational{1, 1000000}, encoder_->time_base);
            duration = (std::min)(duration, end - frame->pts);
        }
        if (duration > 0) {
            if (!video_ && duration < frame->nb_samples) frame->nb_samples = static_cast<int>(duration);
            frame->duration = duration;
            frame->pict_type = AV_PICTURE_TYPE_NONE;
            if (video_) {
                frame->color_range = encoder_->color_range; frame->colorspace = encoder_->colorspace;
                frame->color_primaries = encoder_->color_primaries; frame->color_trc = encoder_->color_trc;
            }
            durations_[frame->pts] = duration;
            SendEncoded(frame);
            if (video_) ++metrics_.videoFrames;
            else metrics_.audioSamples += frame->nb_samples;
        }
        av_frame_unref(frame);
    }
}
// SendEncoded及Finish用它循环取出编码器成品包，按PTS找回之前记录的有效时长，再用rescale_ts换成MP4轨道时间单位。
// DTS描述解码先后顺序，同轨必须递增；检查后交Mp4Output::WritePacket封装，最后解除包引用以便复用。
int TranscodeTrack::ReceiveEncoded() {
    for (;;) {
        CheckCancel();
        int status;
        { ScopedSeconds time(metrics_.encodeSeconds); status = avcodec_receive_packet(encoder_, encoded_.get()); }
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return status;
        Check(status, "Receive export encoded packet");
        auto duration = durations_.find(encoded_->pts);
        if (duration != durations_.end()) { encoded_->duration = duration->second; durations_.erase(duration); }
        av_packet_rescale_ts(encoded_.get(), encoder_->time_base, destination_->time_base);
        if (encoded_->dts == AV_NOPTS_VALUE || (lastDts_ != AV_NOPTS_VALUE && encoded_->dts <= lastDts_))
            throw std::runtime_error("Invalid encoded DTS ordering");
        lastDts_ = encoded_->dts; // AAC的起始负DTS可表示编码预热，不等同于源时间错误
        encoded_->stream_index = destination_->index;
        { ScopedSeconds time(metrics_.muxSeconds); output_.WritePacket(encoded_.get()); }
        av_packet_unref(encoded_.get());
    }
}
// ReceiveFiltered把有效帧交给它，用avcodec_send_frame送编码，再ReceiveEncoded取包；Finish会送空帧表示输入结束。
// 如果编码器暂时不能接收，先取走现成包再重送当前帧，不能把未接收的画面或声音丢掉。
void TranscodeTrack::SendEncoded(AVFrame* frame) {
    int status;
    { ScopedSeconds time(metrics_.encodeSeconds); status = avcodec_send_frame(encoder_, frame); }
    if (status == AVERROR(EAGAIN)) {
        ReceiveEncoded();
        ScopedSeconds time(metrics_.encodeSeconds); status = avcodec_send_frame(encoder_, frame);
    }
    Check(status, "Send export encoder frame");
    ReceiveEncoded();
}
// ExportMp4读完文件后调用它。先向解码器送空包并ReceiveDecoded，再向过滤器送空输入并ReceiveFiltered，
// 最后SendEncoded送空帧、ReceiveEncoded取完编码包，每一级都确认到EOF才继续。
// 这样B帧、滤镜和编码器暂存的尾部内容才不会丢失；外层随后才能给整个MP4写文件尾并发布。
void TranscodeTrack::Finish() {
    CheckCancel();
    int status;
    { ScopedSeconds time(metrics_.decodeSeconds); status = avcodec_send_packet(decoder_, nullptr); }
    if (status == AVERROR(EAGAIN)) { ReceiveDecoded(); status = avcodec_send_packet(decoder_, nullptr); }
    Check(status, "Drain export decoder");
    if (ReceiveDecoded() != AVERROR_EOF) throw std::runtime_error("Export decoder did not drain");
    if (graph_ && !filterEnded_) {
        { ScopedSeconds time(metrics_.filterSeconds); status = av_buffersrc_add_frame_flags(filterSource_, nullptr, 0); }
        if (status != AVERROR_EOF) Check(status, "Drain export filters");
        if (ReceiveFiltered() != AVERROR_EOF) throw std::runtime_error("Export filters did not drain");
    }
    SendEncoded(nullptr);
    if (ReceiveEncoded() != AVERROR_EOF) throw std::runtime_error("Export encoder did not drain");
}
}
