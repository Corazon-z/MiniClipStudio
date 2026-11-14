// 临时透明覆盖层接收拖动；DPI上下文仅在选择期间切换，结果始终保存为物理像素。
#include "ScreenRegionSelector.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#pragma comment(lib, "Gdi32.lib")
namespace miniclip {
namespace {
struct PhysicalDpi {
    DPI_AWARENESS_CONTEXT previous = nullptr;
    PhysicalDpi() {
        previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!previous) throw std::runtime_error("Set screen selector DPI context failed");
    }
    ~PhysicalDpi() { SetThreadDpiAwarenessContext(previous); }
};
struct Selection {
    RECT desktop{};
    POINT start{}, end{};
    bool dragging = false, finished = false;
    std::optional<ScreenRegion> result;
};
// 鼠标点来自GetCursorPos，同线程DPI感知保证它和最终gdigrab偏移都是物理坐标。
void ReadEnd(Selection& selection) {
    if (!GetCursorPos(&selection.end)) throw std::runtime_error("Read screen selection cursor failed");
}
LRESULT CALLBACK RegionProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<Selection*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        data = static_cast<Selection*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    if (!data) return DefWindowProcW(window, message, wParam, lParam);
    try {
        switch (message) {
        case WM_LBUTTONDOWN:
            ReadEnd(*data); data->start = data->end; data->dragging = true;
            SetCapture(window); InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE:
            if (data->dragging) { ReadEnd(*data); InvalidateRect(window, nullptr, FALSE); }
            return 0;
        case WM_LBUTTONUP:
            if (data->dragging) {
                ReadEnd(*data); data->dragging = false; ReleaseCapture();
                data->result = NormalizeScreenRegion(data->start, data->end, data->desktop);
                if (data->result) data->finished = true;
                else InvalidateRect(window, nullptr, FALSE); // 无效范围允许重新拖选。
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { data->finished = true; data->result.reset(); }
            return 0;
        case WM_RBUTTONDOWN: case WM_CLOSE:
            data->finished = true; data->result.reset(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
            RECT client{}; GetClientRect(window, &client);
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            std::wstring text = L"拖动左键选择录屏区域（16—4096像素，宽高自动收为偶数）；Esc或右键取消";
            TextOutW(dc, 24, 24, text.c_str(), static_cast<int>(text.size()));
            if (data->dragging) {
                RECT box{std::min(data->start.x, data->end.x) - data->desktop.left,
                    std::min(data->start.y, data->end.y) - data->desktop.top,
                    std::max(data->start.x, data->end.x) - data->desktop.left,
                    std::max(data->start.y, data->end.y) - data->desktop.top};
                FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            }
            EndPaint(window, &paint); return 0;
        }
        }
    } catch (...) { data->finished = true; data->result.reset(); }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
std::optional<ScreenRegion> NormalizeScreenRegion(POINT start, POINT end, const RECT& desktop) {
    const int64_t left = std::max(std::min(start.x, end.x), desktop.left);
    const int64_t top = std::max(std::min(start.y, end.y), desktop.top);
    const int64_t right = std::min(std::max(start.x, end.x), desktop.right);
    const int64_t bottom = std::min(std::max(start.y, end.y), desktop.bottom);
    if (right <= left || bottom <= top) return {};
    const int64_t width = (right - left) & ~int64_t{1}, height = (bottom - top) & ~int64_t{1};
    if (width < 16 || height < 16 || width > 4096 || height > 4096 || width * height > 16000000) return {};
    return ScreenRegion{static_cast<int>(left), static_cast<int>(top), static_cast<int>(width), static_cast<int>(height)};
}
RECT PhysicalDesktopBounds() {
    PhysicalDpi dpi;
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return RECT{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}
// 选择期间只处理UI消息；不启动媒体工作、不更改系统缩放或隐私设置。
std::optional<ScreenRegion> SelectScreenRegion(HWND owner) {
    PhysicalDpi dpi;
    Selection data; data.desktop = PhysicalDesktopBounds();
    WNDCLASSW type{}; type.lpfnWndProc = RegionProcedure; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MiniClipStudioRegionSelector"; type.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register screen selector failed");
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, type.lpszClassName, L"选择录屏区域",
        WS_POPUP, data.desktop.left, data.desktop.top, data.desktop.right - data.desktop.left,
        data.desktop.bottom - data.desktop.top, owner, nullptr, type.hInstance, &data);
    if (!window) throw std::runtime_error("Create screen selector failed");
    const bool enabled = owner && IsWindowEnabled(owner);
    if (!SetLayeredWindowAttributes(window, 0, 96, LWA_ALPHA)) { DestroyWindow(window); throw std::runtime_error("Set screen selector transparency failed"); }
    if (enabled) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window); SetFocus(window);
    MSG message{}; int code = 1;
    while (!data.finished && (code = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (GetCapture() == window) ReleaseCapture();
    DestroyWindow(window);
    if (enabled && IsWindow(owner)) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (code == 0) { PostQuitMessage(static_cast<int>(message.wParam)); return {}; }
    if (code < 0) throw std::runtime_error("Screen selector message loop failed");
    return data.result;
}
}
