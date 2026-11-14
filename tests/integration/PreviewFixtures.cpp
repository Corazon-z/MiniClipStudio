// 这里为S05制作可定位的已知NUT素材。H.264用固定GOP和B帧，让测试必须从关键帧前置解码。
// 视频PTS由我们逐帧设置，灰度与帧编号一一对应；两条音轨的不同频率用来发现选错轨或seek后混入旧声音。
// ShortAudio只写2秒音频而保留4秒画面，专门验证音频先结束时，播放器仍能补静音并走完视频时间。
#define NOMINMAX
#include "PreviewFixtures.h"
#include "../../src/media/ffmpeg/FfmpegSupport.h"
#include <cstring>
#include <filesystem>
#include <map>
extern "C" {
#include <libavutil/opt.h>
}
using namespace miniclip;
namespace {
constexpr AVRational VideoTimeBase{1, 50};
constexpr AVRational AudioTimeBase{1, 44100};
constexpr AVRational Microseconds{1, 1000000};
constexpr int FrameCount = 100;

struct FixtureResources {
    AVFormatContext* output = nullptr;
    AVCodecContext* encoder = nullptr;
    // 生成成功或异常退出都归还编码器、打开的文件和容器；明确写尾成功仍由主流程检查。
    ~FixtureResources() {
        avcodec_free_context(&encoder);
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
        }
    }
};

// VFR前98帧交替20/60ms，末两帧40ms，让整个视频仍恰好4秒，尾帧也可由25fps后备时长验证。
int64_t PictureTime(int index, bool variableRate) {
    if (!variableRate || index >= 98) return index * 2;
    return (index / 2) * 4 + (index % 2);
}

// PCM轨道直接写已知样本，按写头后的实际time_base换算。第二轨频率不同，不能误选成主音轨。
void WriteAudioFixture(AVFormatContext* output, int streamIndex, int block, bool delayFirstTrack) {
    Packet packet = NewPacket();
    Check(av_new_packet(packet.get(), 1764 * 2), "Allocate preview fixture audio packet");
    auto* samples = reinterpret_cast<int16_t*>(packet->data);
    for (int i = 0; i < 1764; ++i) {
        const int absolute = block * 1764 + i;
        const int frequency = streamIndex == 2 ? 1760 : (absolute < 88200 ? 440 : 880);
        samples[i] = static_cast<int16_t>(6000 * std::sin(2 * 3.141592653589793 * frequency * absolute / 44100));
    }
    packet->stream_index = streamIndex;
    packet->pts = packet->dts = 22050 + block * 1764 + (delayFirstTrack && streamIndex == 1 ? 8820 : 0);
    packet->duration = 1764;
    packet->flags = AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(packet.get(), AudioTimeBase, output->streams[streamIndex]->time_base);
    Check(av_interleaved_write_frame(output, packet.get()), "Write preview fixture audio packet");
}
}

// 先创建视频/音轨和写容器头，再按已知PTS送100帧；编码器重排后的包仍按原PTS找回各帧真实时长。
// 统一加0.5秒保证所有DTS非负，避免封装器为负时间自动平移；读取后的公共origin仍由产品按元数据确定。
void MakePreviewFixture(const std::wstring& file, PreviewFixtureKind kind) {
    if (std::filesystem::exists(file)) throw std::runtime_error("Preview fixture already exists");
    const bool withAudio = kind != PreviewFixtureKind::SilentVideo;
    const bool variableRate = kind == PreviewFixtureKind::VariableRate;
    const bool delayedAudio = kind == PreviewFixtureKind::DelayedAudio;
    FixtureResources resources;
    Check(avformat_alloc_output_context2(&resources.output, nullptr, "nut", Utf8(file).c_str()), "Create preview NUT fixture");
    if (!resources.output) throw std::bad_alloc();
    resources.output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) throw std::runtime_error("Preview fixture requires libx264");
    resources.encoder = avcodec_alloc_context3(codec);
    if (!resources.encoder) throw std::bad_alloc();
    auto* encoder = resources.encoder;
    encoder->width = 64; encoder->height = 48;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = VideoTimeBase; encoder->framerate = AVRational{25, 1};
    encoder->thread_count = 1; encoder->gop_size = 25; encoder->max_b_frames = 2;
    encoder->color_range = AVCOL_RANGE_MPEG; encoder->colorspace = AVCOL_SPC_BT709;
    encoder->color_primaries = AVCOL_PRI_BT709; encoder->color_trc = AVCOL_TRC_BT709;
    if (resources.output->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    Check(av_opt_set(encoder->priv_data, "preset", "veryfast", 0), "Set preview fixture preset");
    Check(av_opt_set(encoder->priv_data, "crf", "10", 0), "Set preview fixture quality");
    Check(av_opt_set(encoder->priv_data, "x264-params", "scenecut=0:keyint=25:min-keyint=25:bframes=2:b-adapt=0:open-gop=0", 0),
        "Set preview fixture GOP and B frames");
    Check(avcodec_open2(encoder, codec, nullptr), "Open preview fixture encoder");
    if (encoder->has_b_frames <= 0) throw std::runtime_error("Preview fixture encoder did not enable B-frame delay");
    AVStream* video = avformat_new_stream(resources.output, nullptr);
    if (!video) throw std::bad_alloc();
    video->time_base = VideoTimeBase; video->avg_frame_rate = AVRational{25, 1};
    Check(avcodec_parameters_from_context(video->codecpar, encoder), "Set preview fixture video parameters");
    if (withAudio) {
        for (int i = 0; i < 2; ++i) {
            AVStream* audio = avformat_new_stream(resources.output, nullptr);
            if (!audio) throw std::bad_alloc();
            auto* parameters = audio->codecpar;
            parameters->codec_type = AVMEDIA_TYPE_AUDIO; parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
            parameters->sample_rate = 44100; parameters->format = AV_SAMPLE_FMT_S16;
            parameters->bits_per_coded_sample = 16; parameters->block_align = 2; parameters->bit_rate = 44100 * 16;
            av_channel_layout_default(&parameters->ch_layout, 1);
            audio->time_base = AudioTimeBase;
        }
    }
    Check(avio_open(&resources.output->pb, Utf8(file).c_str(), AVIO_FLAG_WRITE), "Open preview fixture output");
    Check(avformat_write_header(resources.output, nullptr), "Write preview fixture header");
    Frame frame = NewFrame();
    frame->format = encoder->pix_fmt; frame->width = encoder->width; frame->height = encoder->height;
    frame->color_range = encoder->color_range; frame->colorspace = encoder->colorspace;
    frame->color_primaries = encoder->color_primaries; frame->color_trc = encoder->color_trc;
    Check(av_frame_get_buffer(frame.get(), 32), "Allocate preview fixture picture");
    Packet packet = NewPacket();
    std::map<int64_t, int64_t> durations;
    int packets = 0, keyframes = 0;
    const int64_t offset = av_rescale_q(500000, Microseconds, VideoTimeBase);

    // send_frame之后反复取包；按原PTS补duration，再给PTS/DTS一起加偏移，不能破坏编码器的解码顺序。
    auto receive = [&]() {
        for (;;) {
            const int code = avcodec_receive_packet(encoder, packet.get());
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
            Check(code, "Receive preview fixture encoded packet");
            const auto duration = durations.find(packet->pts);
            if (duration == durations.end() || packet->dts == AV_NOPTS_VALUE)
                throw std::runtime_error("Preview fixture encoder returned unexpected timestamps");
            packet->duration = duration->second; durations.erase(duration);
            packet->pts += offset; packet->dts += offset;
            if (packet->dts < 0) throw std::runtime_error("Preview fixture timestamp offset is insufficient");
            if (packet->flags & AV_PKT_FLAG_KEY) ++keyframes;
            ++packets;
            packet->stream_index = video->index;
            av_packet_rescale_ts(packet.get(), VideoTimeBase, video->time_base);
            Check(av_interleaved_write_frame(resources.output, packet.get()), "Write preview fixture video packet");
            av_packet_unref(packet.get());
        }
    };

    for (int i = 0; i < FrameCount; ++i) {
        Check(av_frame_make_writable(frame.get()), "Make preview fixture picture writable");
        for (int y = 0; y < frame->height; ++y)
            std::memset(frame->data[0] + y * frame->linesize[0], 32 + 2 * i, frame->width);
        for (int plane = 1; plane < 3; ++plane)
            for (int y = 0; y < frame->height / 2; ++y)
                std::memset(frame->data[plane] + y * frame->linesize[plane], 128, frame->width / 2);
        frame->pts = PictureTime(i, variableRate);
        frame->duration = (i + 1 < FrameCount ? PictureTime(i + 1, variableRate) : 200) - frame->pts;
        durations[frame->pts] = frame->duration;
        int code = avcodec_send_frame(encoder, frame.get());
        if (code == AVERROR(EAGAIN)) { receive(); code = avcodec_send_frame(encoder, frame.get()); }
        Check(code, "Send preview fixture picture");
        receive();
        if (withAudio && (kind != PreviewFixtureKind::ShortAudio || i < 50)) {
            WriteAudioFixture(resources.output, 1, i, delayedAudio);
            WriteAudioFixture(resources.output, 2, i, false);
        }
    }
    int code = avcodec_send_frame(encoder, nullptr);
    if (code == AVERROR(EAGAIN)) { receive(); code = avcodec_send_frame(encoder, nullptr); }
    Check(code, "Drain preview fixture encoder");
    if (receive() != AVERROR_EOF || packets != FrameCount || keyframes < 4 || !durations.empty())
        throw std::runtime_error("Preview fixture did not retain all pictures and keyframes");
    Check(av_write_trailer(resources.output), "Finish preview fixture NUT");
    Check(avio_closep(&resources.output->pb), "Close preview fixture output");
}
