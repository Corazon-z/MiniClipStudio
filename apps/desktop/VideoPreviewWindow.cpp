// 这个文件把S05的VideoPlayer接到一个独立Win32预览窗口。主窗口点击“打开视频”时非阻塞地创建它，
// --preview则复用相同窗口过程并运行自己的消息循环。解码线程只PostMessage通知，绘制仍由GUI完成。
// 关闭时保留窗口和定时器，先Stop、等TryJoin真实回收，再销毁承载回调目标的HWND。
#include "VideoPreviewWindow.h"
#include "ConsoleStopSignal.h"
#include "../../src/playback/VideoPlayer.h"
#include "../../src/platform/windows/output/VideoSurface.h"
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <stdexcept>
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Gdi32.lib")

namespace miniclip {
namespace {
constexpr wchar_t PreviewClass[] = L"MiniClipStudioVideoPreview";
constexpr UINT FrameReady = WM_APP + 41;
constexpr UINT_PTR StatusTimer = 1;
constexpr int PauseButton = 2101, StopButton = 2102, ReplayButton = 2103;
constexpr int VolumeBox = 2104, SeekEdit = 2105, SeekButton = 2106, StatusEdit = 2107;
constexpr int VolumeLabel = 2108, SeekLabel = 2109;
HWND currentWindow = nullptr; // 仅GUI线程访问；独立CLI和产品窗口一次只创建一个预览。

struct PreviewWindowData {
    VideoPlayer player;
    std::wstring file;
    HWND owner = nullptr;
    bool ownerWasEnabled = false, standalone = false, ownedByWindow = false, initialized = false;
    bool closing = false, pauseRequested = false, allowPictures = true;
    int exitCode = 0;
    RECT viewport{};
    std::shared_ptr<const PreviewPicture> picture;
    BufferedVideoSurface surface;
    std::wstring lastPauseText;
    uint64_t reportedGeneration = 0;
    int64_t reportedPts = -1;
    std::wstring uiError;
};

// 状态、媒体错误和文件路径来自UTF-8模块；显示到宽字符窗口前在这里转换。
std::wstring PreviewWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) return L"错误文字不是有效UTF-8";
    std::wstring text(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), text.data(), count))
        return L"错误文字转换失败";
    return text;
}

// OpenVideoPreviewWindow未收到路径时先同步选文件；这里只选已有输入，不创建或导出文件。
std::wstring ChoosePreviewFile(HWND owner) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner;
    dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrTitle = L"选择本地视频（只读预览第一视频轨和第一音轨）";
    dialog.lpstrFilter = L"媒体文件（按内容识别）\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) return path;
    const DWORD error = CommDlgExtendedError();
    if (error) throw std::runtime_error("Open preview file dialog failed: " + std::to_string(error));
    return {};
}

// WM_CREATE统一建立子控件并检查失败，避免窗口只出现一半按钮。使用系统GUI字体，无需释放该字体。
HWND MakePreviewControl(HWND window, const wchar_t* type, const wchar_t* text, DWORD style, int id) {
    HWND control = CreateWindowW(type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    if (!control) throw std::runtime_error("Create preview control failed");
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

// 建窗和“重新播放”共用这里开始同一个文件；旧线程必须先由Poll回收，不能覆盖仍被借用的会话。
void StartPreviewPlayback(HWND window, PreviewWindowData& data) {
    data.pauseRequested = false; data.allowPictures = true; data.picture.reset(); data.uiError.clear();
    // VideoPlayer合并通知；回调只投递唤醒，不交出帧裸指针，也不在worker调用GDI。
    data.player.Start(data.file, [window] {
        if (!PostMessageW(window, FrameReady, 0, 0)) throw std::runtime_error("Post preview frame notification failed");
    });
    InvalidateRect(window, &data.viewport, FALSE);
}

// 建窗和WM_SIZE都从这里重新计算位置：画面占上方，底部留下控制、跳转与状态区。
// 画面自身的比例和黑边由DrawPreviewPicture决定，不跟着窗口宽高直接拉伸。
void LayoutPreview(HWND window, PreviewWindowData& data) {
    RECT client{}; GetClientRect(window, &client);
    const int width = std::max(0L, client.right - 24);
    const int bottom = std::max(12L, client.bottom - 158);
    data.viewport = {12, 12, 12 + width, bottom};
    const int controls = bottom + 10;
    MoveWindow(GetDlgItem(window, PauseButton), 12, controls, 110, 30, TRUE);
    MoveWindow(GetDlgItem(window, StopButton), 132, controls, 100, 30, TRUE);
    MoveWindow(GetDlgItem(window, ReplayButton), 242, controls, 100, 30, TRUE);
    MoveWindow(GetDlgItem(window, VolumeLabel), 360, controls + 5, 45, 24, TRUE);
    MoveWindow(GetDlgItem(window, VolumeBox), 410, controls + 2, 125, 180, TRUE);
    MoveWindow(GetDlgItem(window, SeekLabel), 12, controls + 44, 80, 25, TRUE);
    MoveWindow(GetDlgItem(window, SeekEdit), 98, controls + 40, 125, 28, TRUE);
    MoveWindow(GetDlgItem(window, SeekButton), 233, controls + 40, 110, 30, TRUE);
    MoveWindow(GetDlgItem(window, StatusEdit), 12, controls + 80, width,
        std::max(0L, client.bottom - controls - 92), TRUE);
    InvalidateRect(window, nullptr, FALSE);
}

// 每次请求或50ms状态轮询后统一启禁控件；自然EOF先等线程join完，才允许Seek重新启动同一文件。
void UpdatePreviewControls(HWND window, PreviewWindowData& data, const VideoPlaybackSnapshot& status) {
    const bool active = data.player.Active();
    const bool terminal = PlaybackFinished(status.state);
    const bool stopping = status.state == PlaybackState::Stopping;
    EnableWindow(GetDlgItem(window, PauseButton), active && !terminal && !stopping && !data.closing);
    EnableWindow(GetDlgItem(window, StopButton), active && !terminal && !stopping && !data.closing);
    EnableWindow(GetDlgItem(window, ReplayButton), !active && terminal && !data.closing);
    const bool canSeek = status.durationUs.has_value() && !data.closing && !stopping &&
        status.state != PlaybackState::Idle && status.state != PlaybackState::Stopped &&
        status.state != PlaybackState::Failed && !(active && terminal);
    EnableWindow(GetDlgItem(window, SeekEdit), canSeek);
    EnableWindow(GetDlgItem(window, SeekButton), canSeek);
    EnableWindow(GetDlgItem(window, VolumeBox), !data.closing);
    const std::wstring pauseText = data.pauseRequested ? L"继续播放" : L"暂停播放";
    if (pauseText != data.lastPauseText) { SetDlgItemTextW(window, PauseButton, pauseText.c_str()); data.lastPauseText = pauseText; }
}

// Poll复制播放状态显示；这里的绘制偏差是WM_PAINT观测，不把调度邮箱的时刻当作真正绘制。
void ShowPreviewStatus(HWND window, PreviewWindowData& data, const VideoPlaybackSnapshot& status) {
    std::wstring text = PreviewWide(PlaybackStateText(status.state));
    if (status.seeking) text += L"（定位中）";
    text += L"；位置 " + std::to_wstring(status.positionUs / 1000000.0) + L" 秒 / ";
    text += status.durationUs ? std::to_wstring(*status.durationUs / 1000000.0) : L"未知";
    text += L" 秒\r\n时钟：";
    text += status.audioMaster ? L"音频设备" : L"无声视频计时";
    text += L"；已调度/首次绘制：" + std::to_wstring(status.scheduledFrames) + L" / " + std::to_wstring(status.paintedFrames);
    text += L"；最大绘制迟到：" + std::to_wstring(status.maxPaintLatenessUs / 1000.0) + L" ms";
    if (!status.message.empty()) text += L"\r\n" + PreviewWide(status.message);
    if (!data.uiError.empty()) text += L"\r\n" + data.uiError;
    if (data.closing) text += L"\r\n正在等待播放线程真正结束，随后关闭窗口。";
    SetDlgItemTextW(window, StatusEdit, text.c_str());
    UpdatePreviewControls(window, data, status);
}

// WM_TIMER只做状态和真实退出观察，视频通知另走FrameReady，避免50ms定时器限制画面帧率。
void PollPreview(HWND window, PreviewWindowData& data) {
    if (data.player.Active()) data.player.TryJoin();
    const auto status = data.player.Snapshot();
    if (status.state == PlaybackState::Failed) data.exitCode = 1;
    ShowPreviewStatus(window, data, status);
    if (data.closing && !data.player.Active()) DestroyWindow(window);
}

// 跳转按钮读取完整秒数，再交给VideoPlayer::Seek切换请求代次。
// 请求成功后立刻清除旧图；暂停时仍保持暂停，等新代次画面到来再显示，旧通知不能把旧图放回来。
void SeekPreview(HWND window, PreviewWindowData& data, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) throw std::runtime_error("Seek seconds must be finite and non-negative");
    data.player.Seek(seconds);
    data.picture.reset(); data.uiError.clear(); data.allowPictures = true;
    InvalidateRect(window, &data.viewport, FALSE);
    ShowPreviewStatus(window, data, data.player.Snapshot());
}

// 关闭请求只发Stop并保留窗口，确保frameReady回调的HWND在工作线程真正结束之前一直有效。
void BeginPreviewClose(HWND window, PreviewWindowData& data) {
    data.closing = true; data.allowPictures = false; data.player.Stop(); data.picture.reset();
    InvalidateRect(window, &data.viewport, FALSE);
    PollPreview(window, data);
}

// BeginPaint/EndPaint必须成对；用局部对象让异常返回也能归还本次绘制会话。
struct PreviewPaint {
    HWND window; PAINTSTRUCT paint{}; HDC dc;
    // WM_PAINT开始时向Windows申请本次更新区域的DC。
    explicit PreviewPaint(HWND target) : window(target), dc(BeginPaint(target, &paint)) {}
    // 即使绘制处理抛异常，也必须结束这次BeginPaint会话。
    ~PreviewPaint() { EndPaint(window, &paint); }
};

// 与原主窗共用GUI线程，但窗口拥有自己的VideoPlayer；异常止于本回调，不越过Windows边界。
LRESULT CALLBACK PreviewWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<PreviewWindowData*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
        if (message == WM_NCCREATE) {
            data = static_cast<PreviewWindowData*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetLastError(0);
            if (!SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data)) && GetLastError()) return FALSE;
            return TRUE;
        }
        if (!data) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_CREATE:
            MakePreviewControl(window, L"BUTTON", L"暂停播放", WS_TABSTOP, PauseButton);
            MakePreviewControl(window, L"BUTTON", L"停止播放", WS_TABSTOP, StopButton);
            MakePreviewControl(window, L"BUTTON", L"重新播放", WS_TABSTOP, ReplayButton);
            MakePreviewControl(window, L"STATIC", L"音量", 0, VolumeLabel);
            MakePreviewControl(window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, VolumeBox);
            MakePreviewControl(window, L"STATIC", L"跳到秒数", 0, SeekLabel);
            MakePreviewControl(window, L"EDIT", L"0", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, SeekEdit);
            MakePreviewControl(window, L"BUTTON", L"跳转", WS_TABSTOP, SeekButton);
            MakePreviewControl(window, L"EDIT", L"正在打开视频…", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, StatusEdit);
            for (const auto* text : {L"0%（静音）", L"25%", L"50%", L"75%", L"100%"})
                SendDlgItemMessageW(window, VolumeBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
            SendDlgItemMessageW(window, VolumeBox, CB_SETCURSEL, 2, 0);
            SendDlgItemMessageW(window, SeekEdit, EM_SETLIMITTEXT, 80, 0);
            LayoutPreview(window, *data);
            if (!SetTimer(window, StatusTimer, 50, nullptr)) throw std::runtime_error("Create preview status timer failed");
            StartPreviewPlayback(window, *data);
            data->initialized = true;
            ShowPreviewStatus(window, *data, data->player.Snapshot());
            return 0;
        case FrameReady: {
            // 通知只让窗口需要重画；真正绘制时再取最新帧，GUI忙时邮箱仍只保留一张图。
            InvalidateRect(window, &data->viewport, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PreviewPaint paint(window);
            if (!paint.dc) return 0;
            RECT client{}; GetClientRect(window, &client);
            // 只清理视频区域以外的窗口背景；不能在每帧前把视频位置刷成浅色。
            if (!ClearAroundPreview(paint.dc, client, data->viewport, GetSysColorBrush(COLOR_BTNFACE)))
                throw std::runtime_error("Draw preview panel background failed");
            // 控件引起的局部重绘不应重复缩放视频、取走通知或虚报“画面已绘制”。
            if (!RectVisible(paint.dc, &data->viewport)) return 0;
            auto latest = data->player.TakePicture(); // 同时确认合并通知，后续新图可再次唤醒GUI。
            if (!data->closing && data->allowPictures && latest && latest->generation == data->player.Generation())
                data->picture = std::move(latest);
            // 即使旧绘制消息晚到，绘制前还要核对代次；Seek已切换的旧图只能变成黑屏。
            if (data->picture && (!data->allowPictures || data->picture->generation != data->player.Generation())) data->picture.reset();
            if (!data->surface.Draw(paint.dc, data->viewport, data->picture.get())) {
                data->uiError = L"GDI画面绘制失败，已停止本次播放。";
                data->exitCode = 1; data->player.Stop();
            } else if (data->picture && data->viewport.right > data->viewport.left && data->viewport.bottom > data->viewport.top
                && (data->reportedGeneration != data->picture->generation || data->reportedPts != data->picture->ptsUs)) {
                data->player.ReportPainted(data->picture->generation, data->picture->ptsUs, data->picture->durationUs);
                data->reportedGeneration = data->picture->generation; data->reportedPts = data->picture->ptsUs;
            }
            return 0;
        }
        case WM_ERASEBKGND: return 1; // WM_PAINT统一填背景和黑边，减少重复擦除闪烁。
        case WM_SIZE: LayoutPreview(window, *data); return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {650, 480}; return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == VolumeBox && HIWORD(wParam) == CBN_SELCHANGE) {
                const LRESULT choice = SendDlgItemMessageW(window, VolumeBox, CB_GETCURSEL, 0, 0);
                if (choice >= 0 && choice <= 4) data->player.SetVolume(static_cast<float>(choice) / 4);
                return 0;
            }
            if (HIWORD(wParam) != BN_CLICKED || data->closing) return 0;
            if (LOWORD(wParam) == PauseButton) {
                data->pauseRequested = !data->pauseRequested; data->player.SetPaused(data->pauseRequested);
            } else if (LOWORD(wParam) == StopButton) {
                data->allowPictures = false; data->player.Stop(); data->picture.reset(); InvalidateRect(window, &data->viewport, FALSE);
            } else if (LOWORD(wParam) == ReplayButton) {
                StartPreviewPlayback(window, *data);
            } else if (LOWORD(wParam) == SeekButton) {
                wchar_t text[96]{}; GetDlgItemTextW(window, SeekEdit, text, static_cast<int>(std::size(text)));
                wchar_t* end = nullptr; const double seconds = wcstod(text, &end);
                if (end == text || *end) throw std::runtime_error("Please enter a complete number of seconds / 请输入完整秒数");
                SeekPreview(window, *data, seconds);
            }
            ShowPreviewStatus(window, *data, data->player.Snapshot());
            return 0;
        case WM_TIMER:
            if (data->standalone && ConsoleStopSignal::TakeRequest()) {
                SendMessageW(window, WM_CLOSE, 0, 0);
                return 0;
            }
            if (wParam == StatusTimer) PollPreview(window, *data);
            return 0;
        case WM_CLOSE: BeginPreviewClose(window, *data); return 0;
        case WM_DESTROY:
            KillTimer(window, StatusTimer);
            if (currentWindow == window) currentWindow = nullptr;
            if (data->owner && data->ownerWasEnabled && IsWindow(data->owner)) {
                EnableWindow(data->owner, TRUE); SetForegroundWindow(data->owner);
            }
            if (data->standalone && data->initialized) PostQuitMessage(data->exitCode);
            return 0;
        case WM_NCDESTROY: {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const bool deleteData = data->ownedByWindow;
            // 正常关窗早已join；系统/入口异常销毁时，VideoPlayer析构仍会Stop并真实join兜底。
            if (deleteData) delete data;
            return DefWindowProcW(window, message, wParam, lParam);
        }
        default: return DefWindowProcW(window, message, wParam, lParam);
        }
    } catch (const std::exception& error) {
        if (data) {
            data->uiError = PreviewWide(error.what());
            SetDlgItemTextW(window, StatusEdit, data->uiError.c_str());
        }
        std::fprintf(stderr, "Preview window: %s\n", error.what());
        if (message == WM_NCCREATE) return FALSE;
        return message == WM_CREATE ? -1 : 0;
    }
}
}

// 主窗按钮直接调用这里创建预览，不运行嵌套消息循环；CLI也先用它建立同一类窗口。
// CreateWindow成功后才把数据所有权交给窗口，失败时局部unique_ptr会回收已构造的VideoPlayer。
HWND OpenVideoPreviewWindow(HWND owner, const std::wstring& file) {
    if (IsVideoPreviewWindowOpen()) { SetForegroundWindow(currentWindow); return currentWindow; }
    const std::wstring selected = file.empty() ? ChoosePreviewFile(owner) : file;
    if (selected.empty()) return nullptr;
    WNDCLASSW type{};
    type.lpfnWndProc = PreviewWindowProc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = PreviewClass;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Register preview window class failed");
    auto data = std::make_unique<PreviewWindowData>();
    data->file = selected; data->owner = owner; data->standalone = owner == nullptr;
    data->ownerWasEnabled = owner && IsWindowEnabled(owner);
    HWND window = CreateWindowExW(0, PreviewClass, L"MiniClipStudio — S05 视频预览", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 720, owner, nullptr, type.hInstance, data.get());
    if (!window) throw std::runtime_error("Create preview window failed");
    data->ownedByWindow = true; data.release();
    currentWindow = window;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW); SetForegroundWindow(window);
    return window;
}

// 原主窗用它决定互斥控件和延后的关窗是否可以继续，只由同一GUI线程观察窗口句柄。
bool IsVideoPreviewWindowOpen() { return currentWindow && IsWindow(currentWindow); }
// 原主窗收到关闭请求时只投递WM_CLOSE，随后继续自己的消息循环，不在这里同步等播放结束。
void RequestVideoPreviewClose() {
    if (IsVideoPreviewWindowOpen() && !PostMessageW(currentWindow, WM_CLOSE, 0, 0))
        throw std::runtime_error("Request preview window close failed");
}
// 主窗和CLI循环先交这里处理Tab等控件导航；普通消息仍由各自DispatchMessage分发。
bool ProcessVideoPreviewMessage(MSG& message) {
    return IsVideoPreviewWindowOpen() && IsDialogMessageW(currentWindow, &message);
}

// --preview没有原主窗的消息循环，所以在建立同一预览窗口后，从这里处理消息直到WM_QUIT。
// GetMessage异常时也销毁窗口，让VideoPlayer析构真实停止/join，不能让局部状态先于线程失效。
int RunVideoPreviewWindow(const std::wstring& file) {
    HWND window = OpenVideoPreviewWindow(nullptr, file);
    if (!window) return 0; // 文件对话框取消，不代表媒体处理失败。
    MSG message{}; int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!ProcessVideoPreviewMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window); // GetMessage失败时由Data析构真实停止/join兜底。
    if (result < 0) throw std::runtime_error("Preview message loop failed");
    return static_cast<int>(message.wParam);
}
}
