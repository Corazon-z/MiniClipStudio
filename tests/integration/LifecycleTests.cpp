// 这是独立测试程序的入口，负责选择S01—S04的测试辅助功能，并检查原有录音的资源和线程收尾。
// wmain根据参数调用素材生成、输出检查或播放测试；只传一个新WAV路径时，执行WAV调用顺序和控制器重启检查。
// 这里不会创建产品窗口。多数检查不需要麦克风，真实输出设备测试由专门命令选择。
#include "../../src/media/mux/WavWriter.h"
#include "../../src/application/RecordingController.h"
#include <filesystem>
#include <type_traits>
#include <iostream>
#include <cstring>
#include "VideoFixtures.h"
#include "TranscodeTests.h"
#include "PlaybackTests.h"
#include "PreviewClockTests.h"
#include "PreviewFixtures.h"
#include "VideoSurfaceTests.h"
#include "VideoPlaybackTests.h"
#include "CameraTests.h"
#include "ScreenCaptureTests.h"
#include "../../src/media/video/VideoFrameExport.h"
using namespace miniclip;
static_assert(!std::is_copy_constructible_v<InputResources>);
static_assert(!std::is_copy_constructible_v<WavWriter>);
static_assert(!std::is_copy_constructible_v<RecordingController>);
// 集成测试用它统一报告断言失败：条件不满足就抛runtime_error，让wmain输出原因并返回非零。
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// 为S01建立一个明确含视频和两条音轨的AVI，再让RecordingController在后台探测它。
// 先用FFmpeg写出已知包并关闭文件，然后Start探测、TryJoin等待，检查返回结果仍有三条轨道。
void MakeProbeFixture(const std::wstring& path) {
    Require(!std::filesystem::exists(path), "Fixture already exists");
    struct Output {
        AVFormatContext* context = nullptr;
        // MakeProbeFixture结束或抛异常时，在这里关闭它打开的文件，再释放输出上下文。
        ~Output() {
            if (context) {
                if (context->pb) avio_closep(&context->pb);
                avformat_free_context(context);
            }
        }
    } output;
    Check(avformat_alloc_output_context2(&output.context, nullptr, "avi", Utf8(path).c_str()), "Create AVI fixture");
    if (!output.context) throw std::bad_alloc();
    // 先声明三条轨道：第0条是16×16 I420视频，另两条是8kHz单声道S16，各自使用正确时间单位。
    for (int index = 0; index < 3; ++index) {
        AVStream* stream = avformat_new_stream(output.context, nullptr);
        if (!stream) throw std::bad_alloc();
        AVCodecParameters* parameters = stream->codecpar;
        if (index == 0) {
            parameters->codec_type = AVMEDIA_TYPE_VIDEO;
            parameters->codec_id = AV_CODEC_ID_RAWVIDEO;
            parameters->codec_tag = 0x30323449; // I420: YUV420平面画面
            parameters->format = AV_PIX_FMT_YUV420P;
            parameters->width = parameters->height = 16;
            stream->time_base = AVRational{1, 2};
            stream->avg_frame_rate = AVRational{2, 1};
        } else {
            parameters->codec_type = AVMEDIA_TYPE_AUDIO;
            parameters->codec_id = AV_CODEC_ID_PCM_S16LE;
            parameters->sample_rate = 8000;
            parameters->format = AV_SAMPLE_FMT_S16;
            parameters->bits_per_coded_sample = 16;
            parameters->block_align = 2;
            parameters->bit_rate = 128000;
            av_channel_layout_default(&parameters->ch_layout, 1);
            stream->time_base = AVRational{1, 8000};
        }
    }
    Check(avio_open(&output.context->pb, Utf8(path).c_str(), AVIO_FLAG_WRITE), "Open AVI fixture");
    Check(avformat_write_header(output.context, nullptr), "Write fixture header");
    // 再准备两张画面及对应静音包，按写头后的时间基换算PTS/DTS，并交给AVI封装器交错写入。
    for (int frame = 0; frame < 2; ++frame) {
        for (int index = 0; index < 3; ++index) {
            Packet packet = NewPacket();
            const bool video = index == 0;
            Check(av_new_packet(packet.get(), video ? 384 : 8000), "Allocate fixture packet");
            std::memset(packet->data, video ? 128 : 0, packet->size);
            packet->stream_index = index;
            packet->pts = packet->dts = frame * (video ? 1 : 4000);
            packet->duration = video ? 1 : 4000;
            packet->flags = AV_PKT_FLAG_KEY;
            // AVI可能改写时间基，始终换算后写入。
            av_packet_rescale_ts(packet.get(), video ? AVRational{1,2} : AVRational{1,8000}, output.context->streams[index]->time_base);
            Check(av_interleaved_write_frame(output.context, packet.get()), "Write fixture packet");
        }
    }
    Check(av_write_trailer(output.context), "Finish fixture");
    Check(avio_closep(&output.context->pb), "Close fixture");
    // 同时验证GUI使用的后台探测分支，而不只测CLI同步调用。
    RecordingController controller;
    auto job = std::make_unique<RecordingJob>();
    job->kind = TaskKind::ProbeMedia;
    job->probeFile = path;
    controller.Start(std::move(job));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!controller.TryJoin()) {
        Require(std::chrono::steady_clock::now() < deadline, "Background probe did not finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(controller.Current()->result.success, "Background probe failed");
    Require(controller.Current()->mediaInfo.streams.size() == 3, "Background probe lost streams");
    controller.ClearFinished();
}

// 这是LifecycleTests的入口。根据参数调用S01—S04的素材生成和检查函数，或执行WAV与控制器断言。
// 普通WAV路径会依次验证预取消、未知字段、非法调用顺序和重入/重启；异常在这里转成失败退出码。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--test-screen-region") { TestScreenRegion(); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--test-screen-capture") { TestScreenCapture(argv[2]); return 0; }
        if (argc == 4 && std::wstring(argv[1]) == L"--test-camera-session") { TestCameraSession(argv[2], argv[3]); return 0; }
        if (argc == 5 && std::wstring(argv[1]) == L"--test-console-recording-stop") {
            const std::wstring event = argv[4];
            if (event != L"ctrl-c" && event != L"ctrl-break") throw std::runtime_error("Unknown console test event");
            TestConsoleRecordingStop(argv[2], argv[3], event == L"ctrl-break"); return 0;
        }
        // S05既检查实际解码数据，也独立检查GDI像素和WASAPI时钟；明确命令才打开真实设备。
        if (argc == 2 && std::wstring(argv[1]) == L"--test-video-surface") { TestVideoSurface(); return 0; }
        if (argc == 2 && std::wstring(argv[1]) == L"--test-preview-clock") { TestPreviewClock(); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--test-video-playback") { TestVideoPlayback(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--measure-video-playback") { MeasureVideoPlayback(argv[2]); return 0; }
        if (argc == 5 && std::wstring(argv[1]) == L"--inspect-preview") {
            InspectPreviewDecoders(argv[2], std::stod(argv[3]), argv[4]); return 0;
        }
        if (argc == 4 && std::wstring(argv[1]) == L"--make-preview-fixture") {
            const std::wstring kind = argv[2];
            if (kind == L"av") MakePreviewFixture(argv[3], PreviewFixtureKind::AudioVideo);
            else if (kind == L"delay") MakePreviewFixture(argv[3], PreviewFixtureKind::DelayedAudio);
            else if (kind == L"silent") MakePreviewFixture(argv[3], PreviewFixtureKind::SilentVideo);
            else if (kind == L"vfr") MakePreviewFixture(argv[3], PreviewFixtureKind::VariableRate);
            else if (kind == L"short") MakePreviewFixture(argv[3], PreviewFixtureKind::ShortAudio);
            else throw std::invalid_argument("Unknown preview fixture kind");
            return 0;
        }
        // 这些测试参数分别进入声音数值、真实设备、转码和延迟视频检查；只有显式播放命令才打开输出设备。
        if (argc == 4 && std::wstring(argv[1]) == L"--decode-playback") { DecodePlaybackFixture(argv[2], argv[3]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--test-playback-device") { TestPlaybackDevice(argv[2]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--make-benchmark-fixture") { MakeTranscodeFixture(argv[2], false, true); return 0; }
        if (argc == 3 && (std::wstring(argv[1]) == L"--make-export-fixture" || std::wstring(argv[1]) == L"--make-export-offset-fixture")) {
            MakeTranscodeFixture(argv[2], std::wstring(argv[1]) == L"--make-export-offset-fixture"); return 0;
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--inspect-export") { InspectTranscode(argv[2]); return 0; }
        if (argc == 4 && std::wstring(argv[1]) == L"--test-export-cancel") { TestTranscodeCancellation(argv[2], argv[3]); return 0; }
        if (argc == 3 && std::wstring(argv[1]) == L"--make-delayed-video") {
            MakeDelayedVideoFixture(argv[2]);
            return 0;
        }
        // 先设置取消再给S02传不存在的文件，检查它是否在访问输入前就返回专用取消异常。
        {
            std::atomic<bool> cancel{true};
            bool rejected = false;
            try { ExportFirstVideoFrame(L"missing.avi", L"must-not-create.png", &cancel); }
            catch (const VideoExportCancelled&) { rejected = true; }
            Require(rejected, "Video pre-cancel must precede file access");
        }
        if (argc == 3 && std::wstring(argv[1]) == L"--make-probe-fixture") {
            MakeProbeFixture(argv[2]);
            return 0;
        }
        // 未知字段用optional/空分母表示，展示时必须说未知，不能捏造0秒。
        MediaInfo unknown;
        MediaStreamInfo unknownAudio;
        unknownAudio.type = "audio";
        unknown.streams.push_back(unknownAudio);
        const std::string unknownReport = FormatMediaInfo(unknown);
        Require(unknownReport.find("未知") != std::string::npos, "Unknown values were hidden");
        Require(unknownReport.find("0.000000 s") == std::string::npos, "Unknown duration became zero");
        std::atomic<bool> probeCancel{true};
        bool cancelled = false;
        try { ProbeMediaFile(L"does-not-exist.wav", &probeCancel); }
        catch (const MediaProbeCancelled&) { cancelled = true; }
        Require(cancelled, "Probe must honour cancellation before file access");
        if (argc != 2) throw std::runtime_error("Provide a new output WAV path");
        std::filesystem::path path(argv[1]);
        Require(!std::filesystem::exists(path), "Test path already exists");
        // 对WavWriter依次尝试未Open就Push/Finish、空帧、重复Open和空输出收尾，检查错误是否按契约报告。
        {
            WavWriter writer(path.wstring());
            bool rejected = false;
            try { writer.Push(nullptr); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Push before Open must fail");
            rejected = false;
            try { writer.Finish(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Finish before Open must fail");
            writer.Open();
            rejected = false;
            try { writer.Push(nullptr); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Null audio frame must fail safely");
            rejected = false;
            try { writer.Open(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Repeated Open must fail");
            rejected = false;
            try { writer.Finish(); } catch (const NoSamples&) { rejected = true; }
            Require(rejected, "Empty output must not publish");
            rejected = false;
            try { writer.Finish(); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Repeated Finish must fail");
        }
        Require(!std::filesystem::exists(path), "Empty output was published");
        // 重复三轮预取消启动→拒绝重入→TryJoin→检查结果→ClearFinished，确认任务真正结束后才能再次使用。
        RecordingController controller;
        for (int i = 0; i < 3; ++i) {
            auto job = std::make_unique<RecordingJob>();
            job->cancel.store(true); // 启动前取消：不打开设备。
            controller.Start(std::move(job));
            bool rejected = false;
            try { controller.Start(std::make_unique<RecordingJob>()); }
            catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Uncleared task must prevent reentry");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!controller.TryJoin()) {
                Require(std::chrono::steady_clock::now() < deadline, "Cancelled task did not finish");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Require(controller.Current()->result.cancelled, "Cancelled result missing");
            controller.ClearFinished();
            Require(!controller.Current(), "Finished task was retained");
        }
        controller.Shutdown();
        controller.Shutdown();
        std::cout << "PASS: ownership, WAV state checks, controller cancellation/reentry/restart\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
