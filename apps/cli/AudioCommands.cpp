// 这个文件实现wmain带参数启动时的命令选择。S01调用探测，S02导出视频首帧，
// S03转码导出，S04播放音轨，S05打开预览窗口；录音、转WAV和测试声音复用S00保存链路。
// 各分支只检查参数、调用已有媒体接口并显示结果，不重复实现解码器或调用ffmpeg程序。
#include "AudioCommands.h"
#include "TranscodeCommands.h"
#include "PlaybackCommands.h"
#include "CameraCommands.h"
#include "../desktop/CameraWindow.h"
#include "../desktop/VideoPreviewWindow.h"
#include "../../src/capture/audio/AudioRecorder.h"
#include "../../src/media/probe/MediaProbe.h"
#include "../../src/media/video/VideoFrameExport.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <thread>
#include <stdexcept>
namespace miniclip {
namespace {
// --convert接到Windows宽字符路径后，用这里转换成FFmpeg输入需要的UTF-8。
// WideCharToMultiByte先计算长度再转换，并检查无效Unicode；强转指针不能代替文字编码转换。
std::string Utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n && !value.empty()) throw std::runtime_error("Invalid Unicode argument");
    std::string result(n, '\0');
    if (n && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr))
        throw std::runtime_error("Argument conversion failed");
    return result;
}
}
// S00录音、文件转WAV和合成声音完成后共用这个结果输出，按48kHz把样本数换成秒。
// 这些早期实验把成功和明确取消都视为退出码0；S03/S04取消返回3，不能混用两套约定。
int PrintResult(const CaptureResult& result) {
    std::printf("%s; samples=%lld, seconds=%.6f\n", result.message.c_str(),
        static_cast<long long>(result.samples), result.samples / 48000.0);
    return result.success || result.cancelled ? 0 : 1;
}
// wmain在argc>1时调用这里，先用argv[1]确定操作，再检查对应参数数目。
// --export和--play进入专用CLI函数；S01/S02直接调用媒体接口；录音相关分支准备参数后共用RunAudioCapture。
int RunAudioCommands(int argc, wchar_t** argv) {
    std::wstring command = argv[1];
    if (command == L"--camera" && argc == 2) return RunCameraWindow();
    if (command == L"--cameras" || command == L"--camera-modes" || command == L"--camera-options" || command == L"--camera-record" || command == L"--camera-simulate" || command == L"--screen-record")
        return RunCameraCommand(argc, argv);
    if (command == L"--export") return RunTranscodeCommand(argc, argv);
    if (command == L"--play") return RunPlaybackCommand(argc, argv);
    // S05使用独立预览窗口，和主窗口的按钮共用同一窗口过程；不把离屏测试冒充可见视频播放。
    if (command == L"--preview") {
        if (argc != 2 && argc != 3) { std::puts("Usage: --preview [INPUT]"); return 2; }
        return RunVideoPreviewWindow(argc == 3 ? argv[2] : L"");
    }
    // S02调用ExportFirstVideoFrame保存第一张画面，但会继续解码整段视频以核对总帧数。
    // 返回后才打印PNG尺寸、解码帧数和首帧时间；这不是实时播放视频。
    if (command == L"--video-frame" && argc == 4) {
        const auto result = ExportFirstVideoFrame(argv[2], argv[3]);
        std::printf("PNG saved / 首帧已保存; width=%d; height=%d; decoded_frames=%lld; stream=%d\n",
            result.width, result.height, static_cast<long long>(result.decodedFrames), result.streamIndex);
        if (result.firstFrameSeconds) std::printf("First frame timestamp: %.6f seconds\n", *result.firstFrameSeconds);
        else std::puts("First frame timestamp: unknown / 未知");
        std::puts("Note: coded orientation, no rotation/SAR correction or HDR tone mapping.");
        return 0;
    }
    // S01先用ProbeMediaFile取得独立的信息单，再用FormatMediaInfo排成文字；不改源文件。
    if (command == L"--probe" && argc == 3) {
        const MediaInfo info = ProbeMediaFile(argv[2]);
        std::printf("%s", FormatMediaInfo(info).c_str());
        return 0;
    }
    // --devices调用ListAudioDevices列出S00录音可用的输入设备，只显示清单，不开始录音。
    if (command == L"--devices" && argc == 2) {
        auto devices = ListAudioDevices();
        for (size_t i = 0; i < devices.size(); ++i)
            std::printf("%zu: %s\n   %s\n", i, devices[i].description.c_str(), devices[i].name.c_str());
        return devices.empty() ? 1 : 0;
    }
    // --synthetic让WriteSyntheticAudio产生已知测试音，再经PrintResult报告WAV保存结果。
    if (command == L"--synthetic" && argc == 3) return PrintResult(WriteSyntheticAudio(argv[2]));
    if (command == L"--synthetic-stereo" && argc == 3) return PrintResult(WriteSyntheticAudio(argv[2], true));
    CaptureOptions options;
    std::atomic<bool> cancel{false};
    std::atomic<int64_t> progress{0};
    // --convert把已有文件的第一音轨送入S00的固定WAV链路，与S03输出MP4的--export不同。
    // 输入路径先转UTF-8，目标路径保留wstring；下面只准备参数，末尾才调用RunAudioCapture。
    if (command == L"--convert" && argc == 4) {
        options.inputFile = Utf8(argv[2]); options.outputFile = argv[3];
    } else if ((command == L"--record" || command == L"--cancel-test") && argc == 5) {
        // 先枚举输入设备并校验序号、时限，再把所选设备复制到本次CaptureOptions。
        // --record由处理函数按时限结束，--cancel-test则由CLI稍后主动发停止请求。
        auto devices = ListAudioDevices();
        // 解析数字后检查消费长度，拒绝1abc等后缀；另外拒绝NaN/无穷和越界时限。
        size_t used = 0;
        unsigned long index = std::stoul(argv[2], &used);
        if (used != wcslen(argv[2]) || index >= devices.size()) throw std::runtime_error("Invalid device index");
        double seconds = std::stod(argv[3], &used);
        if (used != wcslen(argv[3]) || !std::isfinite(seconds) || seconds <= 0 || seconds > 3600)
            throw std::runtime_error("Duration must be in (0,3600]");
        options.device = devices[index].name; options.outputFile = argv[4];
        if (command == L"--record") options.durationSeconds = seconds;
        else {
            CaptureResult result;
            // 这个测试lambda在新线程中调用RunAudioCapture，主CLI稍后写cancel并join。
            // options/cancel/progress/result都必须活到join结束；窗口不能照搬这里的阻塞等待。
            std::thread worker([&] { result = RunAudioCapture(options, cancel, progress); });
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            // 从发出取消到join返回计时，不包括前面的实验等待；实际仍包含未完成的设备操作。
            auto start = std::chrono::steady_clock::now();
            cancel.store(true);
            worker.join();
            std::printf("Stop-to-join: %.3f seconds\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            return PrintResult(result);
        }
    // 预先设取消，用来验证尚未打开设备就返回且不产生文件。
    } else if (command == L"--cancel-before-start" && argc == 3) {
        options.outputFile = argv[2]; cancel.store(true);
    } else {
        std::puts("Usage: --preview [INPUT] | --play INPUT [--volume 0..100] | --export INPUT OUTPUT.mp4 [options] | --video-frame INPUT OUTPUT.png | --probe INPUT | --devices\n"
            "       --record INDEX SECONDS OUTPUT.wav | --cancel-test INDEX SECONDS OUTPUT.wav\n"
            "       --convert INPUT OUTPUT.wav | --synthetic OUTPUT.wav | --synthetic-stereo OUTPUT.wav\n"
            "       --cancel-before-start OUTPUT.wav");
        return 2;
    }
    return PrintResult(RunAudioCapture(options, cancel, progress));
}

}
