// 窗口只选择参数、发后台任务并绘图；50ms定时器取最新图与状态，不在按钮回调里采集或编码。
#define NOMINMAX
#include "CameraWindow.h"
#include "ConsoleStopSignal.h"
#include "../../src/capture/session/CameraSession.h"
#include "../../src/platform/windows/output/VideoSurface.h"
#include <commdlg.h>
#include <cstdio>
#pragma comment(lib, "Comdlg32.lib")
namespace miniclip {
namespace {
constexpr wchar_t ClassName[] = L"MiniClipStudioCamera";
constexpr int CameraBox = 3101, ModeBox = 3102, MicrophoneBox = 3103, Refresh = 3104;
constexpr int Preview = 3105, Record = 3106, Stop = 3107, Countdown = 3108, OutputFps = 3109, Status = 3110;
constexpr int SourceBox = 3111, RegionButton = 3112, StopHotkey = 1;
HWND current = nullptr; // 仅GUI线程访问。
struct WindowData {
    CameraSession session;
    HWND owner = nullptr;
    bool ownerEnabled = false, owned = false, closing = false;
    std::vector<CameraDevice> cameras;
    std::vector<AudioDevice> microphones;
    std::vector<CameraMode> modes;
    std::shared_ptr<const PreviewPicture> picture;
    BufferedVideoSurface surface;
    std::optional<ScreenRegion> region;
    bool stopHotkey = false, restoreAfterCapture = false;
    bool choosingOptions = false; // 系统对话框/拖选有嵌套消息循环，期间延后销毁WindowData。
    RECT viewport{};
};
// 对话框结束前保留窗口数据；Ctrl+C可先记关闭请求，返回后再由Poll真正销毁。
struct OptionDialogScope {
    WindowData& data;
    explicit OptionDialogScope(WindowData& value) : data(value) { data.choosingOptions = true; }
    ~OptionDialogScope() { data.choosingOptions = false; }
};
// 错误、设备名和状态是UTF-8；控件使用UTF-16，不允许用指针强转替换编码转换。
std::wstring Wide(const std::string& text) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return text.empty() ? L"" : L"无效UTF-8状态";
    std::wstring value(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), size)) return L"文字转换失败";
    return value;
}
// 创建控件统一检查；布局调整只影响GUI，媒体线程不持有HDC。
HWND Control(HWND parent, const wchar_t* type, const wchar_t* caption, DWORD style, int id, int x, int y, int w, int h) {
    HWND child = CreateWindowW(type, caption, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (!child) throw std::runtime_error("Create camera control failed");
    return child;
}
// 模式任务完成后重填，不把后台vector的裸地址塞进组合框item data。
void Add(HWND window, int id, const std::string& text) {
    if (SendDlgItemMessageW(window, id, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Wide(text).c_str())) < 0)
        throw std::runtime_error("Fill camera selection failed");
}
// 组合框选项必须同时满足Windows返回值和当前自有vector范围。
int Selected(HWND window, int id, size_t count) {
    LRESULT value = SendDlgItemMessageW(window, id, CB_GETCURSEL, 0, 0);
    if (value < 0 || static_cast<size_t>(value) >= count) throw std::runtime_error("请选择有效设备/模式");
    return static_cast<int>(value);
}
// 共享一处更新区域说明，刷新设备/切换来源时也不能把旧摄像头模式误显示为录屏范围。
void ShowRegion(HWND window, const WindowData& data) {
    SendDlgItemMessageW(window, ModeBox, CB_RESETCONTENT, 0, 0);
    const auto& region = data.region;
    const std::string text = region ? "区域: (" + std::to_string(region->left) + ", " + std::to_string(region->top)
        + ") " + std::to_string(region->width) + "x" + std::to_string(region->height) + "；Ctrl+Shift+F9停止"
        : "请先点击右侧按钮拖选区域；首版区域录屏只保存画面";
    Add(window, ModeBox, text); SendDlgItemMessageW(window, ModeBox, CB_SETCURSEL, 0, 0);
}
// 工作中只能停止；设备查询不可假取消完成，窗口关闭也继续等真正退出。
void Buttons(HWND window, const WindowData& data) {
    const bool idle = !data.session.Active() && !data.closing;
    const bool screen = SendDlgItemMessageW(window, SourceBox, CB_GETCURSEL, 0, 0) == 1;
    for (int id : {SourceBox, Refresh, Countdown, OutputFps}) EnableWindow(GetDlgItem(window, id), idle);
    for (int id : {CameraBox, ModeBox, MicrophoneBox}) EnableWindow(GetDlgItem(window, id), idle && !screen);
    EnableWindow(GetDlgItem(window, RegionButton), idle && screen);
    const bool ready = idle && (screen ? data.region.has_value() : !data.cameras.empty() && !data.modes.empty());
    EnableWindow(GetDlgItem(window, Preview), ready); EnableWindow(GetDlgItem(window, Record), ready);
    EnableWindow(GetDlgItem(window, Stop), data.session.Active() && data.session.Snapshot().task == CameraTask::Capture);
}
// 查询选中摄像头，先清旧模式，避免用户误用上一设备能力。
void Modes(HWND window, WindowData& data) {
    data.modes.clear(); SendDlgItemMessageW(window, ModeBox, CB_RESETCONTENT, 0, 0);
    data.session.QueryModes(data.cameras.at(Selected(window, CameraBox, data.cameras.size())).name);
    Buttons(window, data);
}
// 保存对话框只选择新路径；原子防覆盖仍由Mp4Output在发布时负责。
std::wstring Output(HWND window) {
    wchar_t path[32768]{};
    SYSTEMTIME time{}; GetLocalTime(&time);
    swprintf_s(path, L"Camera-%04u%02u%02u-%02u%02u%02u.mp4", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window;
    dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"MP4 视频\0*.mp4\0\0"; dialog.lpstrDefExt = L"mp4";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&dialog)) {
        if (CommDlgExtendedError()) throw std::runtime_error("Camera save dialog failed");
        return {};
    }
    return path;
}
// 一次新采集先复制所有选项；预览与录制分开开始，不隐含无缝中途切换。
void StartCapture(HWND window, WindowData& data, bool recording) {
    CameraCaptureOptions options;
    const bool screen = SendDlgItemMessageW(window, SourceBox, CB_GETCURSEL, 0, 0) == 1;
    if (screen) {
        if (!data.region) throw std::runtime_error("请先拖选录屏区域");
        options.screenRegion = data.region;
    } else {
        options.camera = data.cameras.at(Selected(window, CameraBox, data.cameras.size())).name;
        options.mode = data.modes.at(Selected(window, ModeBox, data.modes.size()));
    }
    const int microphone = screen ? 0 : Selected(window, MicrophoneBox, data.microphones.size() + 1);
    if (recording && !screen && microphone) options.microphone = data.microphones.at(microphone - 1).name;
    BOOL valid = FALSE;
    options.outputFps = GetDlgItemInt(window, OutputFps, &valid, FALSE);
    if (!valid || options.outputFps < 1 || options.outputFps > 120) throw std::runtime_error("输出帧率需为1—120整数");
    options.countdownSeconds = GetDlgItemInt(window, Countdown, &valid, FALSE);
    if (!valid || options.countdownSeconds > 10) throw std::runtime_error("倒计时需为0—10秒");
    if (recording) {
        OptionDialogScope dialog(data);
        options.outputFile = Output(window);
        if (options.outputFile.empty() || data.closing) return;
    }
    data.picture.reset(); InvalidateRect(window, &data.viewport, FALSE);
    if (screen && recording) {
        if (!RegisterHotKey(window, StopHotkey, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F9))
            throw std::runtime_error("Ctrl+Shift+F9已被占用，无法建立录屏停止快捷键");
        data.stopHotkey = true;
    }
    try { data.session.Start(options); }
    catch (...) { if (data.stopHotkey) UnregisterHotKey(window, StopHotkey); data.stopHotkey = false; throw; }
    Buttons(window, data);
    if (screen && recording) { data.restoreAfterCapture = true; ShowWindow(window, SW_MINIMIZE); }
}
// 定时器在GUI取快照/最新图；join后再处理设备结果，完成状态不代表可以提前销毁线程。
void Poll(HWND window, WindowData& data) {
    if (!data.owner && ConsoleStopSignal::TakeRequest()) {
        SendMessageW(window, WM_CLOSE, 0, 0);
        return; // 空闲时WM_CLOSE会立即销毁WindowData，不能继续使用data。
    }
    if (data.closing && !data.session.Active() && !data.choosingOptions) {
        DestroyWindow(window); return;
    }
    if (auto picture = data.session.TakePicture()) { data.picture = std::move(picture); InvalidateRect(window, &data.viewport, FALSE); }
    auto snapshot = data.session.Snapshot();
    SetDlgItemTextW(window, Status, Wide(snapshot.message).c_str());
    if (data.session.Active() && data.session.TryJoin()) {
        snapshot = data.session.Snapshot(); // 线程可能恰在前一次快照后结束，join后重新读取最终结果。
        if (data.stopHotkey) { UnregisterHotKey(window, StopHotkey); data.stopHotkey = false; }
        if (data.restoreAfterCapture && !data.closing) { data.restoreAfterCapture = false; ShowWindow(window, SW_RESTORE); }
        if (data.closing) { DestroyWindow(window); return; }
        if (snapshot.state == CameraState::Completed && snapshot.task == CameraTask::Devices) {
            data.cameras = snapshot.cameras; data.microphones = snapshot.microphones;
            SendDlgItemMessageW(window, CameraBox, CB_RESETCONTENT, 0, 0);
            SendDlgItemMessageW(window, MicrophoneBox, CB_RESETCONTENT, 0, 0);
            Add(window, MicrophoneBox, "无声录制 / 不启用麦克风");
            for (const auto& device : data.cameras) Add(window, CameraBox, device.description);
            for (const auto& device : data.microphones) Add(window, MicrophoneBox, device.description);
            SendDlgItemMessageW(window, MicrophoneBox, CB_SETCURSEL, 0, 0);
            const bool screen = SendDlgItemMessageW(window, SourceBox, CB_GETCURSEL, 0, 0) == 1;
            if (!data.cameras.empty()) SendDlgItemMessageW(window, CameraBox, CB_SETCURSEL, 0, 0);
            if (screen) ShowRegion(window, data);
            else if (!data.cameras.empty()) Modes(window, data);
        } else if (snapshot.state == CameraState::Completed && snapshot.task == CameraTask::Modes) {
            data.modes = snapshot.modes;
            for (const auto& mode : data.modes) Add(window, ModeBox, mode.description);
            for (size_t i = 0; i < data.modes.size(); ++i) if (data.modes[i].supported) { SendDlgItemMessageW(window, ModeBox, CB_SETCURSEL, i, 0); break; }
        }
    }
    Buttons(window, data);
}
// 所有Windows回调都接住C++异常；WM_CLOSE先停，WM_TIMER确认真实回收后才销毁。
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<WindowData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        data = static_cast<WindowData*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
    }
    try {
        if (!data) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_CREATE:
            Control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST, SourceBox, 12, 10, 190, 160);
            Add(window, SourceBox, "摄像头"); Add(window, SourceBox, "手动区域录屏（无声）");
            SendDlgItemMessageW(window, SourceBox, CB_SETCURSEL, 0, 0);
            Control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, CameraBox, 215, 10, 370, 240);
            Control(window, L"BUTTON", L"刷新设备", 0, Refresh, 600, 10, 130, 28);
            Control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ModeBox, 12, 48, 570, 260);
            Control(window, L"BUTTON", L"拖选录屏区域…", 0, RegionButton, 600, 48, 130, 28);
            Control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, MicrophoneBox, 12, 86, 720, 240);
            Control(window, L"STATIC", L"倒计时(秒)", 0, 3120, 12, 126, 90, 24);
            Control(window, L"EDIT", L"0", WS_BORDER | ES_NUMBER, Countdown, 110, 122, 40, 28);
            Control(window, L"STATIC", L"输出fps", 0, 3121, 170, 126, 70, 24);
            Control(window, L"EDIT", L"30", WS_BORDER | ES_NUMBER, OutputFps, 245, 122, 50, 28);
            Control(window, L"BUTTON", L"只预览", 0, Preview, 315, 122, 110, 28);
            Control(window, L"BUTTON", L"录制MP4…", 0, Record, 440, 122, 130, 28);
            Control(window, L"BUTTON", L"停止并保存", 0, Stop, 585, 122, 145, 28);
            Control(window, L"EDIT", L"正在查询设备…", WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, Status, 12, 162, 720, 28);
            if (!SetTimer(window, 1, 50, nullptr)) throw std::runtime_error("Create camera timer failed");
            return 0;
        case WM_SIZE:
            GetClientRect(window, &data->viewport); data->viewport.top = 202;
            if (data->viewport.bottom < data->viewport.top) data->viewport.bottom = data->viewport.top;
            InvalidateRect(window, nullptr, FALSE); return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == SourceBox && HIWORD(wParam) == CBN_SELCHANGE && !data->session.Active()) {
                const bool screen = SendDlgItemMessageW(window, SourceBox, CB_GETCURSEL, 0, 0) == 1;
                if (screen) {
                    SendDlgItemMessageW(window, MicrophoneBox, CB_SETCURSEL, 0, 0);
                    SetDlgItemTextW(window, OutputFps, L"15");
                    ShowRegion(window, *data);
                } else if (!data->cameras.empty()) Modes(window, *data);
                Buttons(window, *data); return 0;
            }
            if (LOWORD(wParam) == CameraBox && HIWORD(wParam) == CBN_SELCHANGE && !data->session.Active()) Modes(window, *data);
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            if (LOWORD(wParam) == Stop) data->session.Stop();
            if (!data->session.Active() && !data->closing) {
                if (LOWORD(wParam) == Refresh) { data->modes.clear(); SendDlgItemMessageW(window, ModeBox, CB_RESETCONTENT, 0, 0); data->session.QueryDevices(); }
                if (LOWORD(wParam) == Preview) StartCapture(window, *data, false);
                if (LOWORD(wParam) == Record) StartCapture(window, *data, true);
                if (LOWORD(wParam) == RegionButton) {
                    OptionDialogScope dialog(*data);
                    const auto selected = SelectScreenRegion(window);
                    if (selected && !data->closing) {
                        data->region = selected;
                        ShowRegion(window, *data);
                    }
                }
            }
            Buttons(window, *data); return 0;
        case WM_TIMER: Poll(window, *data); return 0;
        case WM_HOTKEY:
            if (wParam == StopHotkey && data->stopHotkey) data->session.Stop();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
            RECT client{}; GetClientRect(window, &client);
            const bool drawn = ClearAroundPreview(dc, client, data->viewport, GetSysColorBrush(COLOR_BTNFACE))
                && data->surface.Draw(dc, data->viewport, data->picture.get()); EndPaint(window, &paint);
            if (!drawn) SetDlgItemTextW(window, Status, L"绘制失败/窗口区域为空"); return 0;
        }
        case WM_CLOSE:
            data->closing = true; data->session.Stop(); Buttons(window, *data);
            if (!data->session.Active() && !data->choosingOptions) DestroyWindow(window); return 0;
        case WM_ERASEBKGND: return 1; // 视频区由完整后台画布覆盖，避免默认浅色擦除插到两帧之间。
        case WM_DESTROY:
            if (data->stopHotkey) UnregisterHotKey(window, StopHotkey);
            KillTimer(window, 1); if (!data->owner) PostQuitMessage(0); return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0); current = nullptr;
            if (data->owner && data->ownerEnabled && IsWindow(data->owner)) EnableWindow(data->owner, TRUE);
            if (data->owned) delete data;
            return DefWindowProcW(window, message, wParam, lParam);
        }
    } catch (const std::exception& error) {
        if (message == WM_CREATE) return -1;
        SetDlgItemTextW(window, Status, Wide(error.what()).c_str());
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}
// CreateWindow成功后窗口才拥有data；后台枚举从返回前启动，不在WM_CREATE等待设备。
HWND OpenCameraWindow(HWND owner) {
    if (IsCameraWindowOpen()) { SetForegroundWindow(current); return current; }
    WNDCLASSW type{}; type.lpfnWndProc = Procedure; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = ClassName; type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register camera window failed");
    auto data = std::make_unique<WindowData>(); data->owner = owner; data->ownerEnabled = owner && IsWindowEnabled(owner);
    HWND window = CreateWindowW(ClassName, L"MiniClipStudio — 摄像头与区域录屏（Ctrl+Shift+F9停止录屏）", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 730, owner, nullptr, type.hInstance, data.get());
    if (!window) throw std::runtime_error("Create camera window failed");
    data->owned = true; auto* borrowed = data.release(); current = window;
    if (owner) EnableWindow(owner, FALSE);
    try { borrowed->session.QueryDevices(); Buttons(window, *borrowed); }
    catch (...) { DestroyWindow(window); throw; }
    ShowWindow(window, SW_SHOW); return window;
}
bool IsCameraWindowOpen() { return current && IsWindow(current); }
void RequestCameraClose() {
    if (IsCameraWindowOpen() && !PostMessageW(current, WM_CLOSE, 0, 0)) throw std::runtime_error("Request camera close failed");
}
bool ProcessCameraMessage(MSG& message) { return IsCameraWindowOpen() && IsDialogMessageW(current, &message); }
// CLI窗口和主窗口共用界面；消息循环异常退出时析构仍真实回收后台线程。
int RunCameraWindow() {
    HWND window = OpenCameraWindow(nullptr);
    MSG message{}; int code;
    while ((code = GetMessageW(&message, nullptr, 0, 0)) > 0)
        if (!ProcessCameraMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (IsWindow(window)) DestroyWindow(window);
    if (code < 0) throw std::runtime_error("Camera message loop failed");
    return static_cast<int>(message.wParam);
}
}
