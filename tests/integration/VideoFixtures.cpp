// 这个文件用FFmpeg的libavcodec和libavformat制作带B帧延迟的测试视频，供S02/S03验证。
// MakeDelayedVideoFixture把8张已知亮度的图送进MPEG-4编码器，接收编码包并保存为AVI或NUT。
// 编码器可能暂存画面，因此最后还要送入空帧并取完包；这让测试能发现收尾漏帧。
#include "VideoFixtures.h"
#include "../../src/media/ffmpeg/FfmpegSupport.h"
#include <filesystem>
#include <cstring>
using namespace miniclip;
namespace {
struct FixtureResources {
    AVFormatContext* output = nullptr;
    AVCodecContext* encoder = nullptr;
    // 素材生成结束或中途抛异常时，在这里先释放编码器，再关闭文件并归还容器上下文。
    // 这样测试失败也不会遗留已打开的夹具资源；文件是否生成成功仍由外层步骤判断。
    ~FixtureResources() {
        avcodec_free_context(&encoder);
        if (output) {
            if (output->pb) avio_closep(&output->pb);
            avformat_free_context(output);
        }
    }
};
}
// 先创建4fps、16×16的MPEG-4编码器，再逐张送入8帧灰度递增的画面，作为S02/S03的标准输入。
// 每次send_frame后调用下面的receive取包；最后送nullptr排空编码器，再写容器尾部并关闭文件。
void MakeDelayedVideoFixture(const std::wstring& path) {
    if (std::filesystem::exists(path)) throw std::runtime_error("Fixture already exists");
    FixtureResources resources;
    // NUT用于需要完整PTS的转码测试；AVI旧样本仍用于S02顺序帧计数。
    const char* format = std::filesystem::path(path).extension() == L".nut" ? "nut" : "avi";
    Check(avformat_alloc_output_context2(&resources.output, nullptr, format, Utf8(path).c_str()), "Create delayed fixture");
    if (!resources.output) throw std::bad_alloc();
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec) throw std::runtime_error("MPEG4 fixture encoder unavailable");
    resources.encoder = avcodec_alloc_context3(codec);
    if (!resources.encoder) throw std::bad_alloc();
    auto* encoder = resources.encoder;
    encoder->width = encoder->height = 16;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = AVRational{1, 4};
    encoder->framerate = AVRational{4, 1};
    encoder->max_b_frames = 2;
    encoder->gop_size = 12;
    encoder->bit_rate = 400000;
    if (resources.output->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    Check(avcodec_open2(encoder, codec, nullptr), "Open delayed encoder");
    AVStream* stream = avformat_new_stream(resources.output, nullptr);
    if (!stream) throw std::bad_alloc();
    stream->time_base = encoder->time_base;
    Check(avcodec_parameters_from_context(stream->codecpar, encoder), "Set fixture parameters");
    Check(avio_open(&resources.output->pb, Utf8(path).c_str(), AVIO_FLAG_WRITE), "Open delayed fixture");
    Check(avformat_write_header(resources.output, nullptr), "Write delayed header");
    Frame frame = NewFrame();
    frame->width = frame->height = 16;
    frame->format = AV_PIX_FMT_YUV420P;
    Check(av_frame_get_buffer(frame.get(), 32), "Allocate delayed picture");
    Packet packet = NewPacket();
    // MakeDelayedVideoFixture每次送图后都调用它，用receive_packet取出当前已编码好的包。
    // 取到包后换算时间戳并写进容器，直到EAGAIN或EOF；一次送图不保证马上得到一个包。
    auto receive = [&] {
        for (;;) {
            int ret = avcodec_receive_packet(encoder, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            Check(ret, "Receive fixture packet");
            av_packet_rescale_ts(packet.get(), encoder->time_base, stream->time_base);
            packet->stream_index = stream->index;
            Check(av_interleaved_write_frame(resources.output, packet.get()), "Write delayed packet");
            av_packet_unref(packet.get());
        }
    };
    // 这里依次填写8张已知亮度的YUV画面，按各平面的linesize找行，再设置PTS、送帧并接收编码包。
    for (int i = 0; i < 8; ++i) {
        Check(av_frame_make_writable(frame.get()), "Make fixture writable");
        for (int y = 0; y < 16; ++y) std::memset(frame->data[0] + y * frame->linesize[0], 32 + i * 20, 16);
        for (int p = 1; p < 3; ++p)
            for (int y = 0; y < 8; ++y) std::memset(frame->data[p] + y * frame->linesize[p], 128, 8);
        frame->pts = i;
        Check(avcodec_send_frame(encoder, frame.get()), "Send fixture picture");
        receive();
    }
    // 8张图已经送完，但编码器里可能还留着延迟结果；先送nullptr并receive，取尽后再写trailer。
    Check(avcodec_send_frame(encoder, nullptr), "Drain fixture encoder");
    receive();
    Check(av_write_trailer(resources.output), "Finish delayed fixture");
    Check(avio_closep(&resources.output->pb), "Close delayed fixture");
}
