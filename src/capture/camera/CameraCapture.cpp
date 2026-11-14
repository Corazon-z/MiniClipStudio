// 摄像头和麦克风放在同一个dshow输入图中，用共同采集时间而不是S05播放时钟对齐。
// 录制复用TranscodeTrack/Mp4Output；预览只保留最新图，不因GUI绘制速度无限积压画面。
#define NOMINMAX
#include "CameraCapture.h"
#include "../../media/encode/TranscodeTrack.h"
#include "../../media/mux/Mp4Output.h"
#include <filesystem>
#include <limits>
extern "C" {
#include <libswscale/swscale.h>
}
namespace miniclip {
namespace {
using Clock = std::chrono::steady_clock;
struct ReadControl {
    const std::atomic<bool>& stop;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(10);
    Clock::time_point limit = Clock::time_point::max();
    // 回调只读本工作线程的期限与外部原子请求；不能强杀不响应中断的驱动。
    static int Interrupt(void* data) {
        const auto& self = *static_cast<ReadControl*>(data);
        const auto now = Clock::now();
        return self.stop.load() || now >= self.deadline || now >= self.limit;
    }
};
struct PreviewConverter {
    SwsContext* scale = nullptr;
    // swscale缓冲属于当前调用，异常和停止时都归还。
    ~PreviewConverter() { sws_freeContext(scale); }
    // 将借用的源Frame复制为自有BGRA；按真实跨度/颜色矩阵读取，不保存源指针。
    PreviewPicture Convert(const AVFrame* frame, AVRational timeBase, int64_t origin) {
        if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096
            || int64_t{frame->width} * frame->height > 16000000 || frame->decode_error_flags)
            throw std::runtime_error("Invalid/oversized camera picture");
        if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67
            || frame->color_primaries == AVCOL_PRI_BT2020)
            throw std::runtime_error("HDR camera conversion is not supported");
        const auto format = static_cast<AVPixelFormat>(frame->format);
        switch (format) {
        case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_NV12: case AV_PIX_FMT_YUYV422: case AV_PIX_FMT_UYVY422:
        case AV_PIX_FMT_BGR24: case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGRA: case AV_PIX_FMT_BGR0: case AV_PIX_FMT_GRAY8: break;
        default: throw std::runtime_error("Camera preview supports software 8-bit pictures only");
        }
        scale = sws_getCachedContext(scale, frame->width, frame->height, format,
            frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scale) throw std::runtime_error("Create camera pixel converter failed");
        int matrix = SWS_CS_DEFAULT;
        switch (frame->colorspace) {
        case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: matrix = SWS_CS_ITU601; break;
        case AVCOL_SPC_UNSPECIFIED: case AVCOL_SPC_RGB: break;
        default: throw std::runtime_error("Unsupported camera color matrix");
        }
        const int* coefficients = sws_getCoefficients(matrix);
        const int full = frame->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P || format == AV_PIX_FMT_YUVJ422P;
        Check(sws_setColorspaceDetails(scale, coefficients, full, coefficients, 1, 0, 1 << 16, 1 << 16), "Set camera colors");
        PreviewPicture result;
        result.width = frame->width; result.height = frame->height;
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        if (pts == AV_NOPTS_VALUE) throw std::runtime_error("Missing camera picture timestamp");
        result.ptsUs = av_rescale_q(pts, timeBase, AV_TIME_BASE_Q) - origin;
        result.bgra.resize(size_t{static_cast<unsigned>(frame->width)} * frame->height * 4);
        uint8_t* data[] = {result.bgra.data(), nullptr, nullptr, nullptr};
        const int lines[] = {frame->width * 4, 0, 0, 0};
        if (sws_scale(scale, frame->data, frame->linesize, 0, frame->height, data, lines) != frame->height)
            throw std::runtime_error("Incomplete camera preview conversion");
        return result;
    }
};
// 设备标识通常没有冒号/引号；禁止特殊解析字符，避免把名字误解释为额外输入。
std::string DeviceToken(const std::string& name) {
    if (name.empty() || name.find_first_of(":\"'\r\n") != std::string::npos)
        throw std::runtime_error("Unsupported/empty DirectShow device identifier");
    return name;
}
// 首批两个流的数据包用同一原点；不能各自减自己的第一PTS而抹掉启动偏移。
int64_t PacketTime(const AVPacket* packet, const AVStream* stream) {
    if (packet->pts == AV_NOPTS_VALUE) throw std::runtime_error("Capture packet has no PTS");
    const int64_t us = av_rescale_q(packet->pts, stream->time_base, AV_TIME_BASE_Q);
    // gdigrab可使用Unix微秒，dshow是图参考时钟；先保留各自合法大数，再减公共origin。
    const int64_t limit = (std::numeric_limits<int64_t>::max)() / 4;
    if (us < -limit || us > limit) throw std::runtime_error("Capture timestamp out of supported range");
    return us;
}
// FFmpeg帧率解析会约分并限制有理数大小。直接传10000000/333333可能被近似，
// 再向下取整回100ns时少1格，落到驱动能力外。选择目标整数间隔的中点并验证往返结果。
std::string CameraRate(int64_t interval) {
    const AVRational rate = av_d2q(10000000.0 / (interval + 0.5), 1001000);
    if (rate.num <= 0 || rate.den <= 0 || int64_t{rate.den} * 10000000 / rate.num != interval)
        throw std::runtime_error("Camera frame interval cannot be represented safely");
    return std::to_string(rate.num) + "/" + std::to_string(rate.den);
}
}
// 阶段顺序刻意写在一个调用里：校验→倒计时→打开/选轨→有限预取共同原点→处理→排空→发布。
CameraCaptureResult RunCameraCapture(const CameraCaptureOptions& options, const std::atomic<bool>& stop,
    const std::function<void(PreviewPicture)>& picture, const std::function<void(const std::string&)>& onStatus) {
    CameraCaptureResult result;
    const bool live = options.testInput.empty(), recording = !options.outputFile.empty();
    const bool screen = options.screenRegion.has_value();
    // gdigrab在物理像素坐标里抓取；只改本worker上下文，结束后恢复，不更改系统DPI设置。
    struct CaptureDpi {
        DPI_AWARENESS_CONTEXT previous = nullptr;
        ~CaptureDpi() { if (previous) SetThreadDpiAwarenessContext(previous); }
    } dpi;
    if (screen) {
        if (!live || !options.microphone.empty()) throw std::runtime_error("Region capture currently records video only; microphone remains available for camera mode");
        dpi.previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!dpi.previous) throw std::runtime_error("Set screen capture DPI context failed");
        const auto& region = *options.screenRegion;
        const RECT desktop = PhysicalDesktopBounds();
        if (region.width < 16 || region.height < 16 || region.width > 4096 || region.height > 4096
            || region.width % 2 || region.height % 2 || int64_t{region.width} * region.height > 16000000
            || region.left < desktop.left || region.top < desktop.top
            || int64_t{region.left} + region.width > desktop.right || int64_t{region.top} + region.height > desktop.bottom)
            throw std::runtime_error("Screen region is outside desktop or violates even-size limits; select again");
    }
    if (options.outputFps < 1 || options.outputFps > 120 || options.countdownSeconds < 0 || options.countdownSeconds > 10
        || !std::isfinite(options.durationSeconds) || options.durationSeconds < 0 || options.durationSeconds > 86400)
        throw std::runtime_error("Invalid camera fps/countdown/duration");
    if (recording) {
        if (std::filesystem::path(options.outputFile).extension() != L".mp4") throw std::runtime_error("Camera output must be .mp4");
        if (std::filesystem::exists(options.outputFile)) throw std::runtime_error("Camera output already exists; choose a new name");
    }
    if (live && !screen && (!options.mode.supported || (options.mode.codec != "rawvideo" && options.mode.codec != "mjpeg" && options.mode.codec != "h264") || options.mode.width <= 0 || options.mode.height <= 0
        || options.mode.width > 4096 || options.mode.height > 4096 || options.mode.frameInterval <= 0))
        throw std::runtime_error("Choose a supported camera mode first");
    auto status = [&](const std::string& text) { if (onStatus) onStatus(text); };
    for (int remaining = options.countdownSeconds; remaining > 0 && !stop.load(); --remaining) {
        status("Countdown / 倒计时: " + std::to_string(remaining));
        for (int i = 0; i < 20 && !stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (stop.load()) { result.stopped = true; return result; }
    ReadControl control{stop};
    InputResources input;
    input.format = avformat_alloc_context();
    if (!input.format) throw std::bad_alloc();
    input.format->interrupt_callback = {ReadControl::Interrupt, &control};
    const AVInputFormat* backend = nullptr;
    std::string url;
    if (screen) {
        avdevice_register_all(); backend = av_find_input_format("gdigrab");
        if (!backend) throw std::runtime_error("FFmpeg SDK has no gdigrab input");
        const auto& region = *options.screenRegion;
        url = "desktop";
        Check(av_dict_set(&input.options, "video_size", (std::to_string(region.width) + "x" + std::to_string(region.height)).c_str(), 0), "Set screen region size");
        Check(av_dict_set_int(&input.options, "offset_x", region.left, 0), "Set screen region left");
        Check(av_dict_set_int(&input.options, "offset_y", region.top, 0), "Set screen region top");
        Check(av_dict_set_int(&input.options, "framerate", options.outputFps, 0), "Set screen capture rate");
        Check(av_dict_set_int(&input.options, "draw_mouse", 1, 0), "Capture screen cursor");
    } else if (live) {
        avdevice_register_all(); backend = av_find_input_format("dshow");
        if (!backend) throw std::runtime_error("DirectShow unavailable");
        url = "video=" + DeviceToken(options.camera);
        if (!options.microphone.empty()) url += ":audio=" + DeviceToken(options.microphone);
        input.format->flags |= AVFMT_FLAG_NONBLOCK;
        // 统一使用DirectShow图参考时钟，避免某些摄像头自带时钟与麦克风时钟不在同一坐标。
        Check(av_dict_set(&input.options, "use_video_device_timestamps", "0", 0), "Set capture timestamps");
        Check(av_dict_set(&input.options, "video_size", (std::to_string(options.mode.width) + "x" + std::to_string(options.mode.height)).c_str(), 0), "Set camera size");
        Check(av_dict_set(&input.options, "framerate", CameraRate(options.mode.frameInterval).c_str(), 0), "Set camera rate");
        if (options.mode.codec == "rawvideo") Check(av_dict_set(&input.options, "pixel_format", options.mode.pixelFormat.c_str(), 0), "Set camera pixels");
        else input.format->video_codec_id = options.mode.codec == "mjpeg" ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_H264;
        Check(av_dict_set(&input.options, "rtbufsize", "16777216", 0), "Bound capture input buffer");
        if (!options.microphone.empty()) Check(av_dict_set(&input.options, "audio_buffer_size", "50", 0), "Set microphone buffer");
    } else {
        url = Utf8(std::filesystem::absolute(options.testInput).wstring());
        Check(av_dict_set(&input.options, "protocol_whitelist", "file", 0), "Restrict test input protocols");
    }
    status(screen ? "Opening screen region / 正在打开区域录屏…" : "Opening camera / 正在打开输入…");
    int code = avformat_open_input(&input.format, url.c_str(), backend, &input.options);
    if (code == AVERROR_EXIT && stop.load()) { result.stopped = true; return result; }
    Check(code, screen ? "Open screen region" : "Open camera/microphone (check privacy permission, selected mode and device busy)");
    if (av_dict_count(input.options)) throw std::runtime_error("Camera input did not accept an option");
    // gdigrab的目标帧率已由我们设置，不必像未知文件那样预读约20帧估帧率。
    // 仍保留一次实际BMP参数探测；完成后再开启非阻塞，避免首帧未到就因EAGAIN退出探测。
    if (screen) input.format->fps_probe_size = 0;
    code = avformat_find_stream_info(input.format, nullptr);
    if (code == AVERROR_EXIT && stop.load()) { result.stopped = true; return result; }
    Check(code, "Probe capture input");
    if (screen) input.format->flags |= AVFMT_FLAG_NONBLOCK;
    AVStream* video = nullptr; AVStream* audio = nullptr;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        auto* stream = input.format->streams[i];
        if (!video && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = stream;
        if (!audio && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = stream;
    }
    if (!video || (!options.microphone.empty() && !audio)) throw std::runtime_error("Required capture stream missing");
    const int expectedWidth = screen ? options.screenRegion->width : options.mode.width;
    const int expectedHeight = screen ? options.screenRegion->height : options.mode.height;
    if (live && (video->codecpar->width != expectedWidth || video->codecpar->height != expectedHeight))
        throw std::runtime_error("Camera negotiated a different size");
    if (live && options.microphone.empty()) audio = nullptr;
    // 所选音视频都出现后才选公共起点；有界预取防设备失声导致无限积压。
    bool gotVideo = false, gotAudio = !audio;
    int64_t origin = (std::numeric_limits<int64_t>::max)();
    std::vector<Packet> pending;
    size_t pendingBytes = 0;
    control.deadline = Clock::now() + std::chrono::seconds(10);
    while (!gotVideo || !gotAudio) {
        if (stop.load()) { result.stopped = true; return result; }
        if (Clock::now() >= control.deadline) throw std::runtime_error("Capture startup timed out waiting for both streams");
        auto packet = NewPacket();
        code = av_read_frame(input.format, packet.get());
        if (code == AVERROR(EAGAIN)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); if (Clock::now() >= control.deadline) throw std::runtime_error("Capture startup timed out"); continue; }
        if (code == AVERROR_EXIT && stop.load()) { result.stopped = true; return result; }
        Check(code, "Read initial capture packet");
        const bool v = packet->stream_index == video->index, a = audio && packet->stream_index == audio->index;
        if (!v && !a) continue;
        origin = (std::min)(origin, PacketTime(packet.get(), v ? video : audio));
        gotVideo |= v; gotAudio |= a;
        pendingBytes += packet->size;
        if (pending.size() >= 512 || pendingBytes > 33554432) throw std::runtime_error("Capture startup buffer limit exceeded");
        pending.push_back(std::move(packet));
    }
    result.originUs = origin;
    PreviewConverter converter;
    auto show = [&](const AVFrame* frame) {
        if (picture) picture(converter.Convert(frame, video->time_base, origin));
        ++result.previewFrames;
    };
    std::unique_ptr<Mp4Output> output;
    std::unique_ptr<TranscodeTrack> videoTrack, audioTrack;
    TranscodeOptions encode;
    encode.framesPerSecond = options.outputFps;
    if (!live && options.durationSeconds > 0) encode.durationSeconds = options.durationSeconds;
    if (recording) {
        output = std::make_unique<Mp4Output>(options.outputFile);
        // stop仅控制输入循环；轨道不借用stop，否则停止时反而无法排空并保存。
        videoTrack = std::make_unique<TranscodeTrack>(video, *output, encode, origin, result.media, nullptr, show);
        if (audio) audioTrack = std::make_unique<TranscodeTrack>(audio, *output, encode, origin, result.media, nullptr);
        output->WriteHeader();
    } else {
        const AVCodec* codec = avcodec_find_decoder(video->codecpar->codec_id);
        if (!codec) throw std::runtime_error("Camera video decoder unavailable");
        input.decoder = avcodec_alloc_context3(codec);
        if (!input.decoder) throw std::bad_alloc();
        Check(avcodec_parameters_to_context(input.decoder, video->codecpar), "Configure camera decoder");
        input.decoder->pkt_timebase = video->time_base; input.decoder->thread_count = 1; input.decoder->max_pixels = 16000000;
        Check(avcodec_open2(input.decoder, codec, nullptr), "Open camera decoder");
    }
    auto frame = NewFrame();
    auto receive = [&] {
        for (;;) {
            int ret = avcodec_receive_frame(input.decoder, frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            Check(ret, "Decode camera preview"); show(frame.get()); av_frame_unref(frame.get());
        }
    };
    auto process = [&](const AVPacket* packet) {
        if (packet->stream_index == video->index) {
            if (videoTrack) videoTrack->PushPacket(packet);
            else {
                int ret = avcodec_send_packet(input.decoder, packet);
                if (ret == AVERROR(EAGAIN)) { receive(); ret = avcodec_send_packet(input.decoder, packet); }
                Check(ret, "Send camera preview packet"); receive();
            }
        } else if (audioTrack && packet->stream_index == audio->index) audioTrack->PushPacket(packet);
    };
    status(recording ? "Recording / 正在预览并录制" : "Previewing / 正在预览（不保存）");
    for (const auto& packet : pending) process(packet.get());
    pending.clear();
    const auto started = Clock::now();
    if (live && options.durationSeconds > 0) control.limit = started + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(options.durationSeconds));
    auto lastVideo = started, lastAudio = started;
    auto packet = NewPacket();
    while (!stop.load() && Clock::now() < control.limit) {
        if (live && (Clock::now() - lastVideo > std::chrono::seconds(10) || (audio && Clock::now() - lastAudio > std::chrono::seconds(10))))
            throw std::runtime_error("Camera/microphone stopped delivering data");
        control.deadline = (std::min)(lastVideo, audio ? lastAudio : lastVideo) + std::chrono::seconds(10);
        code = av_read_frame(input.format, packet.get());
        if (code == AVERROR(EAGAIN)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
        if (code == AVERROR_EXIT && (stop.load() || Clock::now() >= control.limit)) break;
        if (code == AVERROR_EOF && !live) break;
        Check(code, "Read capture input (device disconnected/busy)");
        if (packet->stream_index == video->index) lastVideo = Clock::now();
        if (audio && packet->stream_index == audio->index) lastAudio = Clock::now();
        process(packet.get()); av_packet_unref(packet.get());
    }
    result.stopped = stop.load();
    status("Finishing / 正在排空并保存，请勿强制关闭…");
    if (recording) {
        videoTrack->Finish(); if (audioTrack) audioTrack->Finish();
        if (!result.media.videoFrames || (audio && !result.media.audioSamples)) throw std::runtime_error("Capture produced no usable video/audio");
        output->Finish(); result.media.outputBytes = output->Bytes(); output->Publish(); result.saved = true;
    } else {
        int ret = avcodec_send_packet(input.decoder, nullptr);
        if (ret == AVERROR(EAGAIN)) { receive(); ret = avcodec_send_packet(input.decoder, nullptr); }
        Check(ret, "Drain camera preview"); receive();
    }
    return result;
}
}
