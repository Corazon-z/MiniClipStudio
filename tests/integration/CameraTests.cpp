// 用同步回调的已知像素与真实后台线程验证S06，既不mock编码器，也不依赖ffmpeg命令行。
#include "CameraTests.h"
#include "../../src/capture/session/CameraSession.h"
#include "../../apps/desktop/ConsoleStopSignal.h"
#include <filesystem>
namespace miniclip {
namespace {
// 显式检查在Debug和Release均有效，不依赖C++assert开关。
void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 用实际线程退出而非状态标志回收；超时抛错后析构仍真实join，不丢弃后台资源。
void Join(CameraSession& session) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!session.TryJoin()) {
        if (std::chrono::steady_clock::now() > deadline) { session.Stop(); throw std::runtime_error("Camera session join timed out"); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}
// 四组独立语义：自有像素、预取消、回调失败、可反复启动并收口的真实线程。
void TestCameraSession(const std::wstring& source, const std::wstring& output) {
    CameraCaptureOptions options; options.testInput = source;
    std::atomic<bool> stop{false};
    int pictures = 0; int64_t previous = -1;
    auto result = RunCameraCapture(options, stop, [&](PreviewPicture picture) {
        Require(picture.width == 64 && picture.height == 48, "Unexpected test picture dimensions");
        Require(picture.ptsUs > previous, "Preview picture timestamp did not increase");
        previous = picture.ptsUs;
        const auto& pixel = picture.bgra;
        Require(pixel.size() == 64 * 48 * 4 && pixel[3] == 255, "Preview buffer/alpha invalid");
        Require(std::abs(int(pixel[0]) - pixel[1]) <= 2 && std::abs(int(pixel[1]) - pixel[2]) <= 2, "Gray source changed channels");
        if (pictures++ == 0) Require(std::abs(int(pixel[0]) - 19) <= 4, "First picture is not known limited-range Y=32 content");
    });
    Require(result.previewFrames == 100 && pictures == 100 && !result.saved, "Preview must decode all 100 B-frames without saving");
    options.outputFile = output;
    stop.store(true);
    result = RunCameraCapture(options, stop);
    Require(result.stopped && !result.saved && !std::filesystem::exists(output), "Pre-stop opened output");
    stop.store(false);
    bool threw = false;
    try { RunCameraCapture(options, stop, [](PreviewPicture) { throw std::runtime_error("deliberate camera consumer failure"); }); }
    catch (const std::runtime_error&) { threw = true; }
    Require(threw && !std::filesystem::exists(output), "Callback failure published a recording");
    CameraSession session;
    session.Start(options); Join(session);
    Require(session.Snapshot().state == CameraState::Completed && session.Snapshot().result.saved, "Background camera session failed");
    const auto last = session.TakePicture();
    Require(last && last->ptsUs >= 3900000 && last->bgra.size() == 64 * 48 * 4, "Latest preview not safely owned after thread exit");
    options.outputFile.clear(); options.countdownSeconds = 2;
    session.Start(options); session.Stop(); Join(session);
    Require(session.Snapshot().state == CameraState::Completed && session.Snapshot().result.stopped, "Countdown stop/restart failed");
    options.countdownSeconds = 0; options.testInput += L".missing";
    session.Start(options); Join(session);
    Require(session.Snapshot().state == CameraState::Failed, "Missing input not reported as failure");
}
void TestConsoleRecordingStop(const std::wstring& source, const std::wstring& output, bool controlBreak) {
    DWORD processIds[2]{};
    Require(GetConsoleProcessList(processIds, 2) == 1 && processIds[0] == GetCurrentProcessId(),
        "Console signal test requires its own isolated console");
    ConsoleStopSignal consoleStop;
    CameraCaptureOptions options; options.testInput = source; options.outputFile = output; options.outputFps = 25;
    std::atomic<bool> stop{false};
    int pictures = 0;
    auto result = RunCameraCapture(options, stop, [&](PreviewPicture) {
        if (++pictures != 10) return;
        Require(GenerateConsoleCtrlEvent(controlBreak ? CTRL_BREAK_EVENT : CTRL_C_EVENT, 0) != 0,
            "Generate isolated recording stop signal failed");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool requested = false;
        while (!(requested = ConsoleStopSignal::TakeRequest()) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        Require(requested, "Console handler did not deliver recording stop request");
        stop.store(true); // 对应GUI收到请求后走WM_CLOSE，再由CameraSession::Stop发出停止。
    });
    Require(result.stopped && result.saved && pictures >= 10 && pictures < 100,
        "Console stop did not finalize a partial recording");
    Require(!ConsoleStopSignal::TakeRequest(), "Consumed console signal remained pending");
}

}
