// 命令解析与验证入口；不是另一个媒体引擎。
#include "AudioCommands.h"
#include "../../src/capture/audio/AudioRecorder.h"
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <chrono>
#include <thread>
#include <stdexcept>
namespace miniclip {
namespace {
std::string Utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n && !value.empty()) throw std::runtime_error("Invalid Unicode argument");
    std::string result(n, '\0');
    if (n && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr))
        throw std::runtime_error("Argument conversion failed");
    return result;
}
// 集中更新状态文字；此函数只在GUI线程调用，不让worker直接操作控件。
}
int PrintResult(const CaptureResult& result) {
    std::printf("%s; samples=%lld, seconds=%.6f\n", result.message.c_str(),
        static_cast<long long>(result.samples), result.samples / 48000.0);
    return result.success || result.cancelled ? 0 : 1;
}
// 命令行实验复用同一媒体链路，用于可重复测试，不调用FFmpeg命令行工具。
int RunAudioCommands(int argc, wchar_t** argv) {
    std::wstring command = argv[1];
    // 设备枚举只打印列表，不创建GUI。先检查argc再按位置取argv，避免缺参数访问。
    if (command == L"--devices" && argc == 2) {
        auto devices = ListAudioDevices();
        for (size_t i = 0; i < devices.size(); ++i)
            std::printf("%zu: %s\n   %s\n", i, devices[i].description.c_str(), devices[i].name.c_str());
        return devices.empty() ? 1 : 0;
    }
    if (command == L"--synthetic" && argc == 3) return PrintResult(WriteSyntheticAudio(argv[2]));
    if (command == L"--synthetic-stereo" && argc == 3) return PrintResult(WriteSyntheticAudio(argv[2], true));
    CaptureOptions options;
    std::atomic<bool> cancel{false};
    std::atomic<int64_t> progress{0};
    // 文件转换入口将宽字符路径转为UTF-8，目标路径仍以wstring交给输出端。
    if (command == L"--convert" && argc == 4) {
        options.inputFile = Utf8(argv[2]); options.outputFile = argv[3];
    } else if ((command == L"--record" || command == L"--cancel-test") && argc == 5) {
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
            // 取消实验专用线程：本控制台入口可以等待，和GUI不能长时等待的要求不同。
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
        std::puts("Usage: --devices | --record INDEX SECONDS OUTPUT.wav | --cancel-test INDEX SECONDS OUTPUT.wav\n"
            "       --convert INPUT OUTPUT.wav | --synthetic OUTPUT.wav | --synthetic-stereo OUTPUT.wav\n"
            "       --cancel-before-start OUTPUT.wav");
        return 2;
    }
    return PrintResult(RunAudioCapture(options, cancel, progress));
}

}
