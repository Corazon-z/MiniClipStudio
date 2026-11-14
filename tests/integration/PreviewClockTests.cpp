// 测试程序在当前线程创建WASAPI会话，持续送静音样本，检查IAudioClock换算与暂停/重置语义。
// 设备/驱动不可用时明确失败，不把未运行的硬件检查当作通过；旧S04回归由原测试单独执行。
#include "PreviewClockTests.h"
#include "../../src/platform/windows/output/WasapiOutput.h"
#include "../../src/media/audio/AudioDecode.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
// 每个断言失败都交给LifecycleTests入口报告，不能只打印错误后继续声称通过。
void RequireClock(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
// 按设备当前空位补静音，保持输出连续，不让人为断粮影响时钟推进测试。
void SupplySilence(miniclip::WasapiOutput& output, const std::vector<float>& silence) {
    const UINT32 available = output.Capacity() - output.Padding();
    if (available) output.Write(silence.data(), available, 0);
}
}

// 独立硬件验证先连续播放约300ms，再暂停确认冻结；恢复后Reset，检查待播数据和设备流位置归零。
// 最后在新设备时间段再次启动，证明Reset没有留下旧位置或破坏下一次播放。
void TestPreviewClock() {
    using namespace miniclip;
    WasapiOutput output;
    output.Open();
    output.EnableClock();
    output.EnableClock(); // 按需启用可重复调用，不重复获取接口或更改已有时钟。
    RequireClock(output.ClockSeconds() == 0, "New WASAPI clock is not at zero");
    std::vector<float> silence(static_cast<size_t>(output.Capacity()) * PlaybackChannels, 0.0f);
    SupplySilence(output, silence);
    output.Start();
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(300)) {
        SupplySilence(output, silence);
        output.Wait();
    }
    const double advanced = output.ClockSeconds();
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    RequireClock(advanced >= 0.12 && advanced <= wall + 0.08, "WASAPI position/frequency does not advance in seconds");
    output.Pause();
    const double paused = output.ClockSeconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    RequireClock(std::abs(output.ClockSeconds() - paused) < 0.002, "Paused WASAPI clock did not freeze");
    output.Start();
    const auto resumed = std::chrono::steady_clock::now();
    while (output.ClockSeconds() < paused + 0.06) {
        if (std::chrono::steady_clock::now() - resumed > std::chrono::seconds(3))
            throw std::runtime_error("Resumed WASAPI clock did not advance");
        SupplySilence(output, silence);
        output.Wait();
    }
    output.Pause();
    SupplySilence(output, silence);
    RequireClock(output.Padding() > 0, "Clock reset test has no pending audio to clear");
    output.ResetStopped();
    RequireClock(output.Padding() == 0, "WASAPI Reset did not clear pending audio");
    RequireClock(output.ClockSeconds() == 0, "WASAPI Reset did not clear stream position");
    SupplySilence(output, silence);
    output.Start();
    const auto restarted = std::chrono::steady_clock::now();
    while (output.ClockSeconds() < 0.06) {
        if (std::chrono::steady_clock::now() - restarted > std::chrono::seconds(3))
            throw std::runtime_error("Reset WASAPI stream did not restart");
        SupplySilence(output, silence);
        output.Wait();
    }
    output.ResetStopped();
    RequireClock(output.Padding() == 0 && output.ClockSeconds() == 0, "Second WASAPI reset retained old content");
    std::printf("PASS preview WASAPI clock: advanced=%.6f wall=%.6f paused=%.6f; reset cleared padding/clock; restart passed\n",
        advanced, wall, paused);
}
