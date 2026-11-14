// 以独立色块反查区域偏移/尺寸/像素，不把“程序保存成功”直接当作录屏内容正确。
#define NOMINMAX
#include "ScreenCaptureTests.h"
#include "../../src/capture/camera/CameraCapture.h"
#include <exception>
#include <thread>
#include <cstdio>
namespace miniclip {
namespace {
void RequireScreen(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 测试窗口只显示四个已知色块，没有用户文档、聊天或桌面内容。
LRESULT CALLBACK Pattern(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        const COLORREF colors[] = {RGB(230,30,20), RGB(20,220,40), RGB(30,40,230), RGB(220,220,220)};
        for (int i = 0; i < 4; ++i) {
            RECT rectangle{(i % 2) * 96, (i / 2) * 64, (i % 2 + 1) * 96, (i / 2 + 1) * 64};
            HBRUSH brush = CreateSolidBrush(colors[i]);
            if (brush) { FillRect(dc, &rectangle, brush); DeleteObject(brush); }
        }
        EndPaint(window, &paint); return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}
struct PatternWindow {
    DPI_AWARENESS_CONTEXT dpi = nullptr;
    HWND window = nullptr;
    ~PatternWindow() { if (window) DestroyWindow(window); if (dpi) SetThreadDpiAwarenessContext(dpi); }
};
// 读取BGRA实际颜色，允许少量桌面合成/转换差异；游标最多遮住部分取样点。
bool ColorAt(const PreviewPicture& picture, int x, int y, int red, int green, int blue) {
    const size_t index = (size_t(y) * picture.width + x) * 4;
    return std::abs(int(picture.bgra[index]) - blue) <= 15
        && std::abs(int(picture.bgra[index+1]) - green) <= 15
        && std::abs(int(picture.bgra[index+2]) - red) <= 15;
}
}
void TestScreenRegion() {
    const RECT desktop{-1920,-200,1920,1080};
    auto a = NormalizeScreenRegion(POINT{201,199}, POINT{-101,-103}, desktop);
    RequireScreen(a && a->left == -101 && a->top == -103 && a->width == 302 && a->height == 302, "Reverse drag/negative coordinates failed");
    auto b = NormalizeScreenRegion(POINT{-2000,-300}, POINT{-1819,-99}, desktop);
    RequireScreen(b && b->left == -1920 && b->top == -200 && b->width == 100 && b->height == 100, "Desktop clipping/even rounding failed");
    RequireScreen(!NormalizeScreenRegion(POINT{0,0},POINT{8,8},desktop), "Tiny region accepted");
    RequireScreen(!NormalizeScreenRegion(POINT{2000,0},POINT{2200,200},desktop), "Outside region accepted");
    RequireScreen(!NormalizeScreenRegion(POINT{0,0},POINT{5000,5000},RECT{0,0,6000,6000}), "Oversized region accepted");
    std::puts("PASS: screen reverse drag, negative coordinates, clipping, even dimensions and limits");
}
// 自己的窗口保持在前面，主测试线程泵UI消息，媒体线程用真实FFmpeg gdigrab和编码器。
void TestScreenCapture(const std::wstring& output) {
    PatternWindow fixture;
    fixture.dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RequireScreen(fixture.dpi != nullptr, "Set fixture physical DPI failed");
    const RECT desktop = PhysicalDesktopBounds();
    RequireScreen(desktop.right - desktop.left >= 256 && desktop.bottom - desktop.top >= 192, "Desktop too small for fixture");
    WNDCLASSW type{}; type.lpfnWndProc = Pattern; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"MiniClipScreenFixture";
    RequireScreen(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Register fixture failed");
    const int x = desktop.left + 32, y = desktop.top + 32;
    fixture.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, type.lpszClassName, L"MiniClipStudio screen test pattern", WS_POPUP,
        x, y, 192, 128, nullptr, nullptr, type.hInstance, nullptr);
    RequireScreen(fixture.window != nullptr, "Create screen fixture failed");
    ShowWindow(fixture.window, SW_SHOWNOACTIVATE); UpdateWindow(fixture.window); GdiFlush();
    CameraCaptureOptions options;
    options.screenRegion = ScreenRegion{x+16,y+16,128,96}; options.outputFile = output;
    options.outputFps = 10; options.durationSeconds = 2;
    std::atomic<bool> stop{false}, finished{false};
    std::exception_ptr error;
    CameraCaptureResult result;
    int64_t checked = 0;
    std::thread worker([&] {
        try {
            result = RunCameraCapture(options, stop, [&](PreviewPicture picture) {
                RequireScreen(picture.width == 128 && picture.height == 96, "Captured wrong region dimensions");
                int correct = 0;
                correct += ColorAt(picture,8,8,230,30,20);
                correct += ColorAt(picture,120,8,20,220,40);
                correct += ColorAt(picture,8,88,30,40,230);
                correct += ColorAt(picture,120,88,220,220,220);
                RequireScreen(correct >= 3, "Actual screen pixels/region offset did not match known pattern");
                ++checked;
            });
        } catch (...) { error = std::current_exception(); }
        finished.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool timedOut = false;
    while (!finished.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (std::chrono::steady_clock::now() >= deadline) { timedOut = true; stop.store(true); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    worker.join(); // 即使超时也确认线程实际退出，资源仍活到这里。
    if (error) std::rethrow_exception(error);
    RequireScreen(!timedOut && result.saved && checked >= 10 && result.media.videoFrames >= 10 && result.media.audioSamples == 0,
        "Screen recording did not finish valid silent MP4");
    std::printf("PASS: actual gdigrab region pixels, %lld preview frames, %lld encoded frames\n",
        static_cast<long long>(checked),static_cast<long long>(result.media.videoFrames));
}
}
