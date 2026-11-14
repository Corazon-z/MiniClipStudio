// 这里实现已有录音和--convert使用的WAV保存链路。RunAudioCapture把解码帧交给Push，
// 本模块用libswresample转换规格，libavcodec生成PCM_S16LE包，再由libavformat封装并完成文件收尾。
// PCM编码是把样本按目标字节格式组织起来，不像S03的AAC那样压缩；S04播放则直接输出PCM，不写WAV。
// 写入器拥有输出/编码器/swr，输入帧只是借用，AVStream随输出上下文一起释放。
#include "WavWriter.h"
namespace miniclip {
// AudioRecorder创建会话时先检查目标路径，生成同目录临时名，但还不获取FFmpeg资源。
// 后续Open才实际写文件，Finish成功才改为最终名，避免把未完成录音显示成成品。
WavWriter::WavWriter(const std::wstring& destination) : destination_(destination) {
        if (destination.empty()) throw std::runtime_error("Empty output path");
        // 先写同目录临时文件；仅当收尾成功才改为用户目标名，避免半成品显示为成功文件。
		temporary_ = destination + L".part-" + std::to_wstring(GetCurrentProcessId())  //进程ID区分常见运行实例，名称仍不是严格唯一标识
			+ L"-" + std::to_wstring(GetTickCount64());    // 毫秒计数降低名称碰撞概率，但不提供严格独占创建保证
    }

// 录音或转换离开作用域时归还swr、源布局、编码器、IO和容器，异常返回也走这里。
// 未发布的临时文件会被清理，但析构不补做Finish，也不能据此宣布保存成功。
WavWriter::~WavWriter() {
		swr_free(&resampler_);
		av_channel_layout_uninit(&inputLayout_);
		avcodec_free_context(&encoder_);
        if (output_) {
            if (output_->pb) {
                int ret = avio_closep(&output_->pb);
                if (ret < 0) std::fprintf(stderr, "Output cleanup close error: %d\n", ret);
            }
            avformat_free_context(output_);
        }
        // 尚未发布则清理临时文件；清理失败记录诊断。强制结束/断电不保证执行析构。
        if (!published_ && !DeleteFileW(temporary_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            std::fprintf(stderr, "Partial file cleanup error: %lu\n", GetLastError());
    }

// RunAudioCapture在处理首个音频包前调用Open，先创建WAV容器与PCM_S16LE编码器，
// 再建输出流、复制编码参数、打开临时文件并写header。只有全部成功才允许Push，不能重复尝试打开。
void WavWriter::Open() {
    if (openAttempted_) throw std::logic_error("WavWriter::Open called twice");
    openAttempted_ = true;
        std::string path = Utf8(temporary_);
        // 临时文件扩展名不是.wav，所以创建容器时明确指定wav，而不是让FFmpeg按扩展名猜。
        Check(avformat_alloc_output_context2(&output_, nullptr, "wav", path.c_str()), "Create WAV");
        if (!output_) throw std::bad_alloc();
        // 查找实现描述是借用，不自行释放；真正有运行状态的是随后分配的encoder_。
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (!codec) throw std::runtime_error("PCM S16LE encoder unavailable");
        encoder_ = avcodec_alloc_context3(codec);
        if (!encoder_) throw std::bad_alloc();
        // 最终声音固定为每声道每秒48000样本、16位有符号整数、左右双声道，源数据仍按实际格式读取。
        encoder_->sample_rate = 48000;
        encoder_->sample_fmt = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&encoder_->ch_layout, 2);
        // 时间单位为1/48000秒；例如PTS=480代表0.01秒。这里不是字节计数。
        encoder_->time_base = AVRational{1, 48000};
        // 容器要求全局编码头时设置GLOBAL_HEADER，保持编码器与容器说明一致。
        if (output_->oformat->flags & AVFMT_GLOBALHEADER) encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        Check(avcodec_open2(encoder_, codec, nullptr), "Open PCM encoder");
        // 输出流随output_管理，stream_只是借用。复制编码参数使容器知道如何描述输出数据。
        stream_ = avformat_new_stream(output_, nullptr);
        if (!stream_) throw std::bad_alloc();
        stream_->time_base = encoder_->time_base;
        Check(avcodec_parameters_from_context(stream_->codecpar, encoder_), "Set output parameters");
        // 先打开临时文件，再写header；参数设定成功不代表磁盘写入一定成功。
        Check(avio_open(&output_->pb, path.c_str(), AVIO_FLAG_WRITE), "Open output file");
        Check(avformat_write_header(output_, nullptr), "Write WAV header");
        opened_ = true;
    }

// AudioRecorder每收到一个解码帧就调用Push。首帧带来真实采样率/格式/布局，
// 因此这时才配置swr；之后核对规格未变，再交Convert转换和写入，不能拿目标格式猜源数据。
void WavWriter::Push(const AVFrame* input) {
    if (!opened_ || finishing_) throw std::logic_error("WavWriter::Push outside recording");
    if (!input) throw std::invalid_argument("WavWriter::Push requires an audio frame");
        if (input->sample_rate <= 0 || input->nb_samples <= 0 || !av_channel_layout_check(&input->ch_layout))
            throw std::runtime_error("Invalid decoded audio parameters");
        // 第一帧才知道真实样本格式/布局，用它建立持续使用的重采样器，不能每帧丢掉转换状态。
        if (!resampler_) {
            inputRate_ = input->sample_rate;
            inputFormat_ = input->format;
            Check(av_channel_layout_copy(&inputLayout_, &input->ch_layout), "Copy input layout");
            // 复制源布局后用swr_alloc_set_opts2同时声明源/目标规格，swr_init成功后供后续帧持续复用。
            Check(swr_alloc_set_opts2(&resampler_, &encoder_->ch_layout, encoder_->sample_fmt, 48000,
                &inputLayout_, static_cast<AVSampleFormat>(inputFormat_), inputRate_, 0, nullptr), "Allocate resampler");
            Check(swr_init(resampler_), "Initialize resampler");
            char layout[128] = {};
            Check(av_channel_layout_describe(&inputLayout_, layout, sizeof(layout)), "Describe layout");
            const char* format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(inputFormat_));
            std::printf("Decoded input: %d Hz, %s, %s -> 48000 Hz, s16, stereo\n",
                inputRate_, format ? format : "unknown", layout);
        // 后续参数变化则明确失败；动态重配置还没有实现，不能继续用旧格式解释新数据。
        } else if (inputRate_ != input->sample_rate || inputFormat_ != input->format ||
            av_channel_layout_compare(&inputLayout_, &input->ch_layout) != 0) {
            throw std::runtime_error("Input format changed during recording; restart recording");
        }
        Convert(input);
    }

// AudioRecorder先排完输入解码器，再调用Finish取出swr延迟样本，Send空帧排完编码器，
// 随后写trailer并关闭IO，最后用MoveFileExW不覆盖发布。任一步失败都留给上层报告，不能报保存成功。
void WavWriter::Finish() {
    if (!opened_ || finishing_) throw std::logic_error("WavWriter::Finish outside recording");
    finishing_ = true;
        // 空输入让重采样滤波器交出延迟样本，否则声音结尾可能比实际采集内容短。
        if (resampler_) while (Convert(nullptr) > 0) {}
        if (samples_ == 0) throw NoSamples();
        // 空帧向编码器表示输入结束，取出内部剩余编码包。
        Send(nullptr);
        Check(av_write_trailer(output_), "Write WAV trailer");
        Check(avio_closep(&output_->pb), "Close output file");
        // 不覆盖已有文件，也不覆盖选择路径后被其他程序创建的文件。
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Publish WAV failed (destination may exist), Windows error " + std::to_string(GetLastError()));
        // 只有最终发布成功才设置标志；若任何一步抛异常，析构仍按失败路径处理临时文件。
        published_ = true;
    }

// 返回已提交编码的每声道有效样本数，供AudioRecorder换算时长；这里数的不是字节或缓冲容量。
int64_t WavWriter::Samples() const { return samples_; }

// Push和Finish通过Convert执行重采样：用swr_get_delay估算目标容量，准备输出帧后调用swr_convert，
// 只把真实产生的样本送给Send，并以累计样本数设置PTS。空输入时继续取尾部，返回0表示本轮没有剩余样本。
int WavWriter::Convert(const AVFrame* input) {
        // 容量向上舍入：(内部延迟+输入样本)×目标采样率/输入采样率。容量不是实际有效数据量。
        int64_t capacity = av_rescale_rnd(swr_get_delay(resampler_, inputRate_)
            + (input ? input->nb_samples : 0), 48000, inputRate_, AV_ROUND_UP);
        if (capacity > INT_MAX) throw std::runtime_error("Audio buffer too large");
        // 智能指针拥有本次输出帧；函数结束或中途异常都会释放。此实现仍有重复分配，尚待测量优化。
        Frame frame = NewFrame();
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = 48000;
        // 至少留256样本空间用于排空；容量不代表有效长度，max括号写法避开Windows宏。
        frame->nb_samples = static_cast<int>((std::max)(capacity, int64_t{256}));
        Check(av_channel_layout_copy(&frame->ch_layout, &encoder_->ch_layout), "Copy output layout");
        Check(av_frame_get_buffer(frame.get(), 0), "Allocate output samples");
        // extended_data是一组声道/平面地址，支持planar；不是只拷贝data[0]。
        // 这里的const_cast匹配C接口，不会把数据内容转换成另一种音频格式；转换由swr完成。
        int produced = swr_convert(resampler_, frame->extended_data, frame->nb_samples,
			input ? const_cast<const uint8_t**>(input->extended_data) : nullptr, input ? input->nb_samples : 0);
        Check(produced, "Resample audio");
        if (produced) {
            // 只编码实际有效样本；若把容量当长度，会将无效缓冲尾部也作为音频。
            frame->nb_samples = produced;
            // 新帧从此前累计样本位置开始；送入编码器成功后再累加本帧样本数。
            frame->pts = samples_;
            Send(frame.get());
            samples_ += produced;
        }
		return produced;
    }

// Send交帧后调用Receive，把avcodec_receive_packet返回的包全部取走；每包先换成输出流时间单位，
// 再交av_interleaved_write_frame写WAV。EAGAIN表示本轮暂时没有包，EOF表示编码结束。
void WavWriter::Receive() {
        Packet packet = NewPacket();
        for (;;) {
            int ret = avcodec_receive_packet(encoder_, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            Check(ret, "Receive PCM packet");
            packet->stream_index = stream_->index;
            // 把编码器时间单位换算为输出流时间单位，不能直接假定所有环节单位相同。
            av_packet_rescale_ts(packet.get(), encoder_->time_base, stream_->time_base);
            // 该写包接口会消耗包引用；包对象继续由unique_ptr管理，随后unref空引用安全。
            Check(av_interleaved_write_frame(output_, packet.get()), "Write WAV packet");
            av_packet_unref(packet.get());
        }
    }

// Convert用Send向编码器交样本，Finish交空帧通知结束。若send_frame返回EAGAIN，
// 先由Receive取出结果腾空间，再重送同一帧，最后接收全部可用包，不能直接跳过未接收的输入。
void WavWriter::Send(const AVFrame* frame) {
        int ret = avcodec_send_frame(encoder_, frame);
        if (ret == AVERROR(EAGAIN)) { Receive(); ret = avcodec_send_frame(encoder_, frame); }
        Check(ret, "Send PCM frame");
        Receive();
    }
}
