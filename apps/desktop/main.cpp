// 产品桌面入口：Win32窗口与状态显示；媒体与线程实现位于src。
// 界面与任务管理入口。GUI线程只更新窗口/请求取消；worker处理媒体。
// 阅读顺序：Job → StartJob → StartRecording → Poll → WndProc → wmain。
// NOMINMAX防止Windows头里的min/max宏干扰标准库函数。
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#include <process.h>
#include <cstdio>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
#include "../../src/capture/audio/AudioRecorder.h"
#include "../../src/application/RecordingController.h"
#include "../cli/AudioCommands.h"
using namespace miniclip;
// 链接Windows保存对话框所需的导入库；与FFmpeg库是不同依赖。
#pragma comment(lib, "Comdlg32.lib")
// 内部辅助名称限制在当前翻译单元。控件ID区分按钮/组合框；TIMER区分定时器消息。
namespace {
constexpr int ID_RECORD = 1001, ID_DEVICE = 1002, ID_REFRESH = 1003, ID_STATUS = 1004;
constexpr UINT_PTR TIMER = 1;
// 一次后台任务的数据。GUI拥有Job，worker借用；退出并join前不能销毁。
using Job = miniclip::RecordingJob;
miniclip::RecordingController g_controller;
std::vector<AudioDevice> g_devices;
bool g_closing = false;
// 把UTF-8结果/设备名称转为Windows宽字符串：先查长度，再分配并转换。
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) return L"无法显示错误文本（无效UTF-8）";
    std::wstring result(n, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n))
        return L"UTF-8转换失败";
    return result;
}
// 把宽字符命令行路径转为UTF-8输入路径；类型转换指针不能替代编码转换。
void Status(HWND hwnd, const std::wstring& text) { SetDlgItemTextW(hwnd, ID_STATUS, text.c_str()); }
// 根据Job是否存在、任务种类、取消和关闭意图，统一启用/禁用控件并更新按钮文字。
void Controls(HWND hwnd) {
    bool busy = static_cast<bool>(g_controller.Current());
    EnableWindow(GetDlgItem(hwnd, ID_DEVICE), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_REFRESH), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_RECORD), !g_closing &&
        (busy ? !g_controller.Current()->enumerate && !g_controller.Current()->cancel.load() : !g_devices.empty()));
    SetDlgItemTextW(hwnd, ID_RECORD, busy ? (g_controller.Current()->cancel.load() ? L"正在停止…" : L"停止并保存") : L"开始录音…");
}
// 接收任务所有权并启动线程；调用者保证当前没有另一个未回收任务。
// 只把任务交给控制器；窗口不再自己创建/回收线程。
void StartJob(HWND hwnd, std::unique_ptr<Job> job) {
    try { g_controller.Start(std::move(job)); }
    catch (const std::exception& error) { Status(hwnd, L"创建任务失败：" + Wide(error.what())); }
    Controls(hwnd);
}
// 枚举可能耗时，也走后台任务；make_unique创建并拥有新的Job。
void Refresh(HWND hwnd) {
    auto job = std::make_unique<Job>();
    job->enumerate = true;
    Status(hwnd, L"正在后台枚举麦克风设备…");
    StartJob(hwnd, std::move(job));
}
// 按钮开始路径：取设备选择 → 选新文件 → 写入不可变参数 → 启动worker。
void StartRecording(HWND hwnd) {
    // 组合框可能尚无选择；检查CB_ERR和范围后才取vector元素，防止越界。
    LRESULT index = SendDlgItemMessageW(hwnd, ID_DEVICE, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<size_t>(index) >= g_devices.size()) return;
    // 生成易识别的建议文件名；局部数组由保存对话框同步使用，之后复制到options。
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t path[32768]{};
    swprintf_s(path, L"Recording-%04u%02u%02u-%02u%02u%02u.wav", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    // {}将结构清零，再填写结构大小、所属窗口、过滤器和输出缓冲。W结尾接口使用宽字符。
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    // 过滤器按“说明、模式”用空字符分隔，并以双空字符结束，不是普通单段文本。
    dialog.lpstrFilter = L"WAV 音频 (*.wav)\0*.wav\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"wav";
    dialog.lpstrTitle = L"选择新 WAV 文件（不会覆盖已有文件）";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    // 对话框返回false可能只是用户取消；CommDlgExtendedError非0才是对话框错误。
    if (!GetSaveFileNameW(&dialog)) {
        DWORD error = CommDlgExtendedError();
        if (error) Status(hwnd, L"保存对话框失败：" + std::to_wstring(error));
        return;
    }
    // 这里先保护已有文件；最终发布还会再次拒绝覆盖，防选择后其他程序创建同名文件的竞态。
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd, L"为保护已有录音，请选择一个新的文件名。", L"文件已存在", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto job = std::make_unique<Job>();
    // 复制设备标识和路径，不让worker借用即将失效的局部数组或可变化的列表元素。
    job->options.device = g_devices[index].name;
    job->options.outputFile = path;
    Status(hwnd, L"正在打开设备；停止后完成 WAV 保存。实际输入格式见控制台。\r\n输出：48kHz / S16 / 双声道");
    StartJob(hwnd, std::move(job));
}
// GUI定时器回调中的任务观察：只做零等待检查，不在界面线程长时间等录音结束。
void Poll(HWND hwnd) {
    if (!g_controller.Current()) return;
    if (!g_controller.TryJoin()) {
        if (!g_controller.Current()->enumerate) {
            std::wstring text = g_controller.Current()->cancel.load() ? L"正在停止并收尾，请等待线程真正退出…" : L"正在启动/录音；点击“停止并保存”结束。";
            text += L"\r\n已处理音频：" + std::to_wstring(g_controller.Current()->progress.load() / 48000.0) + L" 秒";
            Status(hwnd, text);
        }
        return;
    }
    // TryJoin成功已建立同步；下面才可读取worker写入的普通结果和设备数组。
    if (g_controller.Current()->enumerate) {
        // 将枚举结果交给GUI列表管理，再同步填入组合框；这些控件操作不在worker执行。
        g_devices = std::move(g_controller.Current()->devices);
        SendDlgItemMessageW(hwnd, ID_DEVICE, CB_RESETCONTENT, 0, 0);
        for (const auto& device : g_devices) {
            std::wstring name = Wide(device.description);
            SendDlgItemMessageW(hwnd, ID_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        if (!g_devices.empty()) SendDlgItemMessageW(hwnd, ID_DEVICE, CB_SETCURSEL, 0, 0);
        Status(hwnd, g_controller.Current()->result.success ? (g_devices.empty() ? L"没有音频设备，请连接设备后刷新。" : L"请选择设备，然后开始录音。")
            : L"设备枚举失败：" + Wide(g_controller.Current()->result.message));
    } else {
        std::wstring message = Wide(g_controller.Current()->result.message);
        std::printf("%s; samples=%lld\n", g_controller.Current()->result.message.c_str(), static_cast<long long>(g_controller.Current()->result.samples));
        if (g_controller.Current()->result.success) message = L"保存成功，时长：" + std::to_wstring(g_controller.Current()->result.samples / 48000.0)
            + L" 秒\r\n" + g_controller.Current()->options.outputFile;
        else message = (g_controller.Current()->result.cancelled ? L"已取消：" : L"录音失败：") + message;
        Status(hwnd, message);
    }
    // 确认worker退出后才销毁Job，恢复控件；若用户先前请求关窗，此时才真正销毁窗口。
    g_controller.ClearFinished();
    Controls(hwnd);
    if (g_closing) DestroyWindow(hwnd);
}
// Windows按约定调用的窗口过程。这里只分发消息，耗时工作交worker。异常不能穿过此回调边界。
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        switch (message) {
        // 窗口创建时建立设备框、按钮和状态文本。检查任一创建失败并返回-1使窗口创建失败。
        case WM_CREATE:
            if (!CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                15, 15, 440, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DEVICE)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"刷新设备", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 465, 15, 110, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"开始录音…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 15, 55, 180, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECORD)), nullptr, nullptr) ||
                !CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 15, 105, 560, 130, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), nullptr, nullptr)) return -1;
            // 每50ms请求GUI观察一次任务，定时器不创建新线程，也不是严格实时调度。
            if (!SetTimer(hwnd, TIMER, 50, nullptr)) return -1;
            Refresh(hwnd);
            return 0;
        // 按钮通知：空闲才开始/刷新；运行中按停止只写原子标志，不直接释放媒体上下文。
        case WM_COMMAND:
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            if (LOWORD(wParam) == ID_REFRESH && !g_controller.Current()) Refresh(hwnd);
            if (LOWORD(wParam) == ID_RECORD) {
                if (!g_controller.Current()) StartRecording(hwnd);
                else if (!g_controller.Current()->enumerate) { g_controller.Current()->cancel.store(true); Controls(hwnd); }
            }
            return 0;
        // 定时消息仍由GUI消息循环处理，调用Poll观察进度和完成状态。
        case WM_TIMER:
            if (wParam == TIMER) Poll(hwnd);
            return 0;
        // 用户点关闭时先取消并保留消息循环；若后台正在打开/关闭驱动，继续等待真实退出。
        case WM_CLOSE:
            if (g_controller.Current()) { g_closing = true; g_controller.Current()->cancel.store(true); Controls(hwnd); Status(hwnd, L"正在结束后台任务，完成后自动关闭…"); }
            else DestroyWindow(hwnd);
            return 0;
        // 窗口实际销毁后停止计时器，发WM_QUIT让消息循环结束。
        case WM_DESTROY:
            KillTimer(hwnd, TIMER);
            PostQuitMessage(0);
            return 0;
        // 不处理的系统消息交默认过程，例如窗口移动/尺寸和系统行为。
        default: return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    } catch (const std::exception& error) {
        Status(hwnd, L"操作失败：" + Wide(error.what()));
        return message == WM_CREATE ? -1 : 0;
    }
}
// CLI输出与退出码：正常完成或明确取消返回0，真实错误返回1。
} // namespace
// Windows宽字符入口。无参数开GUI，有参数走CLI；控制台子系统仍可同时创建Win32窗口。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc > 1) return RunAudioCommands(argc, argv);
        HINSTANCE instance = GetModuleHandleW(nullptr);
        // 声明窗口类属性；{}先清零，lpfnWndProc注册回调函数地址，不是此处调用回调。
        WNDCLASSW window{};
        window.lpfnWndProc = WndProc;
        window.hInstance = instance;
        window.lpszClassName = L"MiniClipAudioCapture";
        window.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&window)) throw std::runtime_error("RegisterClass failed");
        // 创建窗口对象；实际控件初始化会在WM_CREATE中完成，失败检查后才显示。
        HWND hwnd = CreateWindowW(window.lpszClassName, L"MiniClipStudio — WAV录音", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 620, 300, nullptr, nullptr, instance, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        MSG message{};
        int ret;
        // 消息循环：正数处理，0正常退出，-1是错误。IsDialogMessage支持控件键盘导航。
        while ((ret = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        // 消息循环异常退出时也不能释放正在使用的Job；真实等待，绝不detach。
        // 异常退出兜底：joinable不是“还在运行”的判断，只表示仍需要回收管理关系。
        // 此异常路径可能真实等待；不能detach或销毁Job冒充安全退出。
        g_controller.Shutdown();
        if (ret < 0) throw std::runtime_error("GetMessage failed");
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
