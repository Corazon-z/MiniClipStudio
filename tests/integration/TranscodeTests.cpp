// 这个文件用FFmpeg制作已知画面和声音，验证S03的选轨、裁剪、转码和取消。
// 测试先生成NUT输入，再调用产品的ExportMp4；InspectTranscode独立解码成品，核对帧数、亮度和结束时间。
// 这些函数编入LifecycleTests，不进入产品程序；它们用来核对结果，不替代产品转码代码。
#include "TranscodeTests.h"
#include "../../src/export/Transcode.h"
#include "../../src/media/ffmpeg/FfmpegSupport.h"
#include <filesystem>
#include <cstring>
#include <limits>
using namespace miniclip;
namespace {
struct FixtureOutput {
    AVFormatContext* value = nullptr;
    // 生成S03测试素材的函数离开作用域时，在这里关闭文件并释放输出上下文。
    // 即使写包或检查过程抛异常，已经取得的资源也会收回。
    ~FixtureOutput() {
        if (value) {
            if (value->pb) avio_closep(&value->pb);
            avformat_free_context(value);
        }
    }
};
}
// 这个函数为S03准备内容已知的NUT输入：普通素材是2秒、25fps、64×48，基准素材为10秒、640×360。
// 先创建视频与两条音轨，再把亮度变化的图和440→880Hz/1760Hz声音写成包；每次写包前换算时间基。
void MakeTranscodeFixture(const std::wstring& path, bool audioOffset, bool benchmark) {
    const int width = benchmark ? 640 : 64, height = benchmark ? 360 : 48;
    const int frames = benchmark ? 250 : 50;
    const int lumaBytes = width * height;
    if (std::filesystem::exists(path)) throw std::runtime_error("Fixture already exists");
    FixtureOutput output;
    Check(avformat_alloc_output_context2(&output.value, nullptr, "nut", Utf8(path).c_str()), "Create export fixture");
    if (!output.value) throw std::bad_alloc();
    for (int i = 0; i < 3; ++i) {
        AVStream* stream = avformat_new_stream(output.value, nullptr);
        if (!stream) throw std::bad_alloc();
        auto* parameters = stream->codecpar;
        if (i == 0) {
            parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
            parameters->codec_tag = 0x30323449; // I420：NUT也要明确原始画面的像素存法
            parameters->width = width; parameters->height = height;
            parameters->format = AV_PIX_FMT_YUV420P;
            stream->time_base = AVRational{1, 25};
            stream->avg_frame_rate = AVRational{25, 1};
        } else {
            parameters->codec_type = AVMEDIA_TYPE_AUDIO;
            parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
            parameters->sample_rate = 44100; parameters->format = AV_SAMPLE_FMT_S16;
            parameters->bits_per_coded_sample = 16; parameters->block_align = 2; parameters->bit_rate = 88200 * 8;
            av_channel_layout_default(&parameters->ch_layout, 1);
            stream->time_base = AVRational{1, 44100};
        }
    }
    Check(avio_open(&output.value->pb, Utf8(path).c_str(), AVIO_FLAG_WRITE), "Open export fixture");
    Check(avformat_write_header(output.value, nullptr), "Write fixture header");
    // 每个视频时刻对应1764个44100Hz声音样本（44100/25），因此画面和声音能覆盖同样的时间。
    // audioOffset会把音轨整体后移8820样本；写头后时间基可能改变，包交给封装器前统一换算。
    for (int frame = 0; frame < frames; ++frame) {
        for (int i = 0; i < 3; ++i) {
            Packet packet = NewPacket();
            Check(av_new_packet(packet.get(), i == 0 ? lumaBytes * 3 / 2 : 3528), "Allocate export fixture packet");
            if (i == 0) {
                if (benchmark) {
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < width; ++x) packet->data[y * width + x] = 32 + (x / 8 + y / 8 + frame * 3) % 160;
                } else std::memset(packet->data, 48 + frame * 2, lumaBytes);
                std::memset(packet->data + lumaBytes, 128, lumaBytes / 2);
                packet->pts = packet->dts = frame;
                packet->duration = 1;
            } else {
                auto* samples = reinterpret_cast<int16_t*>(packet->data);
                for (int sample = 0; sample < 1764; ++sample) {
                    const int absolute = frame * 1764 + sample;
                    const int frequency = i == 2 ? 1760 : (absolute < 44100 ? 440 : 880);
                    samples[sample] = static_cast<int16_t>(6000 * std::sin(2 * 3.141592653589793 * frequency * absolute / 44100));
                }
                packet->pts = packet->dts = frame * 1764 + (audioOffset ? 8820 : 0);
                packet->duration = 1764;
            }
            packet->stream_index = i;
            packet->flags = AV_PKT_FLAG_KEY;
            av_packet_rescale_ts(packet.get(), i == 0 ? AVRational{1,25} : AVRational{1,44100}, output.value->streams[i]->time_base);
            Check(av_interleaved_write_frame(output.value, packet.get()), "Write export fixture packet");
        }
    }
    Check(av_write_trailer(output.value), "Finish export fixture");
    Check(avio_closep(&output.value->pb), "Close export fixture");
}

// S03测试在导出完成后调用它，重新读文件并检查每轨DTS递增、内容末尾和完整解码帧数。
// 视频包交给解码器，下面的receive计算亮度；EOF后继续排空。它按本项目输出契约检查成品。
void InspectTranscode(const std::wstring& path) {
    InputResources input;
    Check(avformat_open_input(&input.format, Utf8(path).c_str(), nullptr, nullptr), "Open inspection input");
    Check(avformat_find_stream_info(input.format, nullptr), "Inspect streams");
    int video = -1;
    for (unsigned i = 0; i < input.format->nb_streams; ++i)
        if (input.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video < 0) video = static_cast<int>(i);
    if (video < 0) throw std::runtime_error("Inspection requires video");
    const AVCodec* decoder = avcodec_find_decoder(input.format->streams[video]->codecpar->codec_id);
    if (!decoder) throw std::runtime_error("Inspection decoder unavailable");
    input.decoder = avcodec_alloc_context3(decoder);
    if (!input.decoder) throw std::bad_alloc();
    Check(avcodec_parameters_to_context(input.decoder, input.format->streams[video]->codecpar), "Set inspection decoder");
    Check(avcodec_open2(input.decoder, decoder, nullptr), "Open inspection decoder");
    std::vector<int64_t> lastDts(input.format->nb_streams, AV_NOPTS_VALUE);
    double videoEnd = 0, audioEnd = 0, firstY = 0, lastY = 0;
    int64_t frames = 0;
    Frame frame = NewFrame(); Packet packet = NewPacket();
    // InspectTranscode用它取尽当前可用的YUV420P画面，沿linesize遍历有效Y像素并计算平均亮度。
    // 第一帧和最后一帧的亮度连同实际帧数会写进检查报告，让测试能够核对画面内容。
    auto receive = [&] {
        for (;;) {
            int ret = avcodec_receive_frame(input.decoder, frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            Check(ret, "Inspect decoded picture");
            if (frame->format != AV_PIX_FMT_YUV420P || frame->decode_error_flags)
                throw std::runtime_error("Unexpected inspected picture");
            double sum = 0;
            for (int y = 0; y < frame->height; ++y)
                for (int x = 0; x < frame->width; ++x) sum += frame->data[0][y * frame->linesize[0] + x];
            lastY = sum / (frame->width * frame->height);
            if (frames++ == 0) firstY = lastY;
            av_frame_unref(frame.get());
        }
    };
    // 这里逐轨比较DTS，用PTS+duration计算包末尾；只将视频包送入解码器，声音包用于核对结束时间。
    for (;;) {
        int ret = av_read_frame(input.format, packet.get());
        if (ret == AVERROR_EOF) break;
        Check(ret, "Inspect packet");
        int index = packet->stream_index;
        if (packet->dts == AV_NOPTS_VALUE || (lastDts[index] != AV_NOPTS_VALUE && packet->dts <= lastDts[index]))
            throw std::runtime_error("Inspection found non-monotonic DTS");
        lastDts[index] = packet->dts;
        const AVStream* stream = input.format->streams[index];
        const double end = (packet->pts + packet->duration) * av_q2d(stream->time_base);
        if (index == video) {
            videoEnd = (std::max)(videoEnd, end);
            Check(avcodec_send_packet(input.decoder, packet.get()), "Inspect send packet"); receive();
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audioEnd = (std::max)(audioEnd, end);
        av_packet_unref(packet.get());
    }
    // 容器包已经读完，还要送空包并接收解码器尾部，才能得出完整画面计数。
    Check(avcodec_send_packet(input.decoder, nullptr), "Inspect drain"); receive();
    std::printf("{\"video_frames\":%lld,\"first_y\":%.3f,\"last_y\":%.3f,\"video_end\":%.6f,\"audio_end\":%.6f}\n",
        static_cast<long long>(frames), firstY, lastY, videoEnd, audioEnd);
}
// 先把cancel设为true调用ExportMp4，再清除标志并通过进度回调在已有画面后重新取消。
// 每次都检查异常类型和目标未发布状态；停止请求交给导出器处理，测试不自行释放其媒体资源。
void TestTranscodeCancellation(const std::wstring& input, const std::wstring& output) {
    TranscodeOptions options; options.inputFile = input; options.outputFile = output;
    std::atomic<bool> cancel{true};
    bool rejected = false;
    try { ExportMp4(options, &cancel); } catch (const TranscodeCancelled&) { rejected = true; }
    if (!rejected || std::filesystem::exists(output)) throw std::runtime_error("Export pre-cancel failed");
    cancel.store(false); rejected = false;
    try {
        // 这个同步回调看到首个输出画面后只设置cancel，让ExportMp4按自己的清理顺序退出。
        ExportMp4(options, &cancel, [&](const TranscodeProgress& value) { if (value.videoFrames > 0) cancel.store(true); });
    } catch (const TranscodeCancelled&) { rejected = true; }
    if (!rejected || std::filesystem::exists(output)) throw std::runtime_error("Export in-flight cancel failed");
    std::puts("PASS: export pre-cancel and cancellation after frames were produced");
}
