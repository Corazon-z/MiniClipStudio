// CLI可独立验证S06，不依赖操作窗口；simulate明确使用测试文件，不把它当真实摄像头证据。
#define NOMINMAX
#include "CameraCommands.h"
#include <Windows.h>
#include <cmath>
#include "../../src/capture/camera/CameraCapture.h"
#include "../../src/capture/DeviceCatalog.h"
#include "../../src/media/ffmpeg/FfmpegSupport.h"
#include <cstdio>
#include <cwchar>
#include <limits>
extern "C" {
#include <libavutil/log.h>
}
namespace miniclip {
namespace {
std::atomic<bool> interrupted{false};
// 控制台回调只发停止请求，编码器排空和文件保存留给采集线程。
BOOL WINAPI OnConsole(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true); return TRUE;
}
struct ConsoleControl {
    ConsoleControl() { interrupted.store(false); if (!SetConsoleCtrlHandler(OnConsole, TRUE)) throw std::runtime_error("Install camera stop handler failed"); }
    ~ConsoleControl() { if (!SetConsoleCtrlHandler(OnConsole, FALSE)) std::fputs("Remove camera stop handler failed\n", stderr); }
};
// 参数必须完整解析且非负；std::stod接受nan/inf，额外的有限值检查不能省略。
double Number(const wchar_t* value) {
    size_t used = 0; const double result = std::stod(value, &used);
    if (used != wcslen(value) || !std::isfinite(result) || result < 0 || result > 86400) throw std::runtime_error("Invalid camera numeric argument");
    return result;
}
int Integer(const wchar_t* value) {
    const double result = Number(value);
    if (std::floor(result) != result) throw std::runtime_error("Camera argument must be an integer");
    return static_cast<int>(result);
}
// 屏幕可位于主屏左/上方，所以坐标允许负数；尺寸/帧率仍使用原非负解析。
int Coordinate(const wchar_t* value) {
    size_t used = 0; const auto number = std::stoll(value, &used);
    if (used != wcslen(value) || number < (std::numeric_limits<int>::min)() || number > (std::numeric_limits<int>::max)())
        throw std::runtime_error("Invalid screen coordinate");
    return static_cast<int>(number);
}
template<class T> const T& At(const std::vector<T>& list, int index) {
    if (static_cast<size_t>(index) >= list.size()) throw std::runtime_error("Camera/device/mode index out of range");
    return list[index];
}
}
// 名字序号只用于本次交互，实际引擎接收复制的唯一名；模式包含真实采集帧间隔。
int RunCameraCommand(int argc, wchar_t** argv) {
    // 诊断只改变当前互斥CLI任务的日志级别，退出时恢复，避免影响后续工具调用。
    struct RestoreLog { int level = av_log_get_level(); ~RestoreLog() { av_log_set_level(level); } } restore;
    const std::wstring command = argv[1];
    if (command == L"--cameras" && argc == 2) {
        const auto devices = ListCameraDevices();
        for (size_t i = 0; i < devices.size(); ++i) std::printf("%zu: %s\n", i, devices[i].description.c_str());
        std::printf("CAMERA_COUNT=%zu\n", devices.size()); return 0;
    }
    if (command == L"--camera-modes" && argc == 3) {
        const auto devices = ListCameraDevices();
        const auto modes = ListCameraModes(At(devices, Integer(argv[2])).name);
        for (size_t i = 0; i < modes.size(); ++i) std::printf("%zu: %s\n", i, modes[i].description.c_str());
        std::printf("MODE_COUNT=%zu\n", modes.size()); return 0;
    }
    // 将FFmpeg自己的驱动能力日志作为交叉诊断；list_options按约定列完即AVERROR_EXIT，不是录制失败。
    if (command == L"--camera-options" && argc == 3) {
        const auto camera = At(ListCameraDevices(), Integer(argv[2]));
        avdevice_register_all(); // 原生设备枚举不注册FFmpeg后端，这个独立CLI分支须自己准备。
        const AVInputFormat* backend = av_find_input_format("dshow");
        if (!backend) throw std::runtime_error("DirectShow input unavailable");
        InputResources input;
        Check(av_dict_set(&input.options, "list_options", "true", 0), "List FFmpeg camera options");
        const int result = avformat_open_input(&input.format, ("video=" + camera.name).c_str(), backend, &input.options);
        if (result != AVERROR_EXIT) { Check(result, "Query FFmpeg camera options"); throw std::runtime_error("Camera options query did not exit as expected"); }
        return 0;
    }
    CameraCaptureOptions options;
    int firstOption = 0;
    const bool simulated = command == L"--camera-simulate";
    if (command == L"--screen-record" && argc >= 4 && std::wstring(argv[2]) == L"--select") {
        const auto region = SelectScreenRegion(nullptr);
        if (!region) return 3;
        options.screenRegion = region; options.outputFile = argv[3]; options.outputFps = 15; firstOption = 4;
    } else if (command == L"--screen-record" && argc >= 7) {
        options.screenRegion = ScreenRegion{Coordinate(argv[2]), Coordinate(argv[3]), Integer(argv[4]), Integer(argv[5])};
        options.outputFile = argv[6]; options.outputFps = 15; firstOption = 7;
    } else if (simulated && argc >= 4) { options.testInput = argv[2]; options.outputFile = argv[3]; firstOption = 4; }
    else if (command == L"--camera-record" && argc >= 5) {
        const auto cameras = ListCameraDevices();
        options.camera = At(cameras, Integer(argv[2])).name;
        options.mode = At(ListCameraModes(options.camera), Integer(argv[3]));
        options.outputFile = argv[4]; firstOption = 5;
    } else {
        std::puts("Usage: --cameras | --camera-modes CAMERA_INDEX | --camera-options CAMERA_INDEX\n"
            "  --camera-record CAMERA_INDEX MODE_INDEX NEW.mp4 [--mic INDEX] [--seconds S] [--fps N] [--countdown N] [--verbose]\n"
            "  --camera-simulate INPUT NEW.mp4 [--seconds S] [--fps N] [--stop-after-frames N] (test input only)");
        std::puts("  --screen-record --select NEW.mp4 [--seconds S] [--fps N] [--countdown N]\n"
            "  --screen-record X Y WIDTH HEIGHT NEW.mp4 [--seconds S] [--fps N] (video only)");
        return 2;
    }
    int stopAfterFrames = 0;
    for (int i = firstOption; i < argc; ++i) {
        const std::wstring flag = argv[i];
        if (flag == L"--verbose") { av_log_set_level(AV_LOG_DEBUG); continue; }
        if (++i >= argc) throw std::runtime_error("Camera option missing value");
        if (flag == L"--seconds") options.durationSeconds = Number(argv[i]);
        else if (flag == L"--fps") options.outputFps = Integer(argv[i]);
        else if (flag == L"--countdown") options.countdownSeconds = Integer(argv[i]);
        else if (flag == L"--mic" && !simulated) options.microphone = At(ListAudioDevices(), Integer(argv[i])).name;
        else if (flag == L"--stop-after-frames" && simulated) stopAfterFrames = Integer(argv[i]);
        else throw std::runtime_error("Unknown camera option");
    }
    ConsoleControl console;
    int64_t seen = 0;
    const auto result = RunCameraCapture(options, interrupted, [&](PreviewPicture picture) {
        ++seen;
        if (picture.bgra.size() != size_t(picture.width) * picture.height * 4) throw std::runtime_error("Bad camera preview buffer");
        if (stopAfterFrames && seen >= stopAfterFrames) interrupted.store(true);
    }, [](const std::string& message) { std::fprintf(stderr, "%s\n", message.c_str()); });
    std::printf("CAMERA_RESULT {\"source\":\"%s\",\"simulated\":%s,\"saved\":%s,\"stopped\":%s,\"preview_frames\":%lld,\"video_frames\":%lld,\"audio_samples\":%lld,\"origin_us\":%lld}\n",
        options.screenRegion ? "screen-region" : simulated ? "test-file" : "camera", simulated ? "true" : "false", result.saved ? "true" : "false", result.stopped ? "true" : "false",
        static_cast<long long>(result.previewFrames), static_cast<long long>(result.media.videoFrames),
        static_cast<long long>(result.media.audioSamples), static_cast<long long>(result.originUs));
    return result.saved ? 0 : 3;
}
}
