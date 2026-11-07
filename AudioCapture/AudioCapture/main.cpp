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
#include "AudioCapture.h"
#pragma comment(lib, "Comdlg32.lib")
namespace {
constexpr int ID_RECORD = 1001, ID_DEVICE = 1002, ID_REFRESH = 1003, ID_STATUS = 1004;
constexpr UINT_PTR TIMER = 1;
struct Job {
    bool enumerate = false;
    CaptureOptions options;
    std::atomic<bool> cancel{false};
    std::atomic<int64_t> progress{0};
    CaptureResult result;
    std::vector<AudioDevice> devices;
};
// Job 由 GUI 拥有；结果仅在线程 join 后读取，原子字段可在运行中访问。
std::unique_ptr<Job> g_job;
std::thread g_worker;
std::vector<AudioDevice> g_devices;
bool g_closing = false;
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) return L"无法显示错误文本（无效UTF-8）";
    std::wstring result(n, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n))
        return L"UTF-8转换失败";
    return result;
}
std::string Utf8(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n && !value.empty()) throw std::runtime_error("Invalid Unicode argument");
    std::string result(n, '\0');
    if (n && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr))
        throw std::runtime_error("Argument conversion failed");
    return result;
}
void Status(HWND hwnd, const std::wstring& text) { SetDlgItemTextW(hwnd, ID_STATUS, text.c_str()); }
void Controls(HWND hwnd) {
    bool busy = static_cast<bool>(g_job);
    EnableWindow(GetDlgItem(hwnd, ID_DEVICE), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_REFRESH), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_RECORD), !g_closing &&
        (busy ? !g_job->enumerate && !g_job->cancel.load() : !g_devices.empty()));
    SetDlgItemTextW(hwnd, ID_RECORD, busy ? (g_job->cancel.load() ? L"正在停止…" : L"停止并保存") : L"开始录音…");
}
void StartJob(HWND hwnd, std::unique_ptr<Job> job) {
    g_job = std::move(job);
    Job* shared = g_job.get();
    try {
        g_worker = std::thread([shared]() {
            try {
                if (shared->enumerate) {
                    shared->devices = ListAudioDevices();
                    shared->result.success = true;
                } else shared->result = RunAudioCapture(shared->options, shared->cancel, shared->progress);
            } catch (const std::exception& error) { shared->result.message = error.what(); }
        });
    } catch (const std::exception& error) {
        g_job.reset();
        Status(hwnd, L"创建线程失败：" + Wide(error.what()));
    }
    Controls(hwnd);
}
void Refresh(HWND hwnd) {
    auto job = std::make_unique<Job>();
    job->enumerate = true;
    Status(hwnd, L"正在后台枚举麦克风设备…");
    StartJob(hwnd, std::move(job));
}
void StartRecording(HWND hwnd) {
    LRESULT index = SendDlgItemMessageW(hwnd, ID_DEVICE, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<size_t>(index) >= g_devices.size()) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t path[32768]{};
    swprintf_s(path, L"Recording-%04u%02u%02u-%02u%02u%02u.wav", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter = L"WAV 音频 (*.wav)\0*.wav\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt = L"wav";
    dialog.lpstrTitle = L"选择新 WAV 文件（不会覆盖已有文件）";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&dialog)) {
        DWORD error = CommDlgExtendedError();
        if (error) Status(hwnd, L"保存对话框失败：" + std::to_wstring(error));
        return;
    }
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd, L"为保护已有录音，请选择一个新的文件名。", L"文件已存在", MB_OK | MB_ICONINFORMATION);
        return;
    }
    auto job = std::make_unique<Job>();
    job->options.device = g_devices[index].name;
    job->options.outputFile = path;
    Status(hwnd, L"正在打开设备；停止后完成 WAV 保存。实际输入格式见控制台。\r\n输出：48kHz / S16 / 双声道");
    StartJob(hwnd, std::move(job));
}
void Poll(HWND hwnd) {
    if (!g_job) return;
    // Windows 线程句柄仅用于零等待探测；std::thread 负责 join 和关闭句柄。
    DWORD wait = WaitForSingleObject(g_worker.native_handle(), 0);
    if (wait == WAIT_TIMEOUT) {
        if (!g_job->enumerate) {
            std::wstring text = g_job->cancel.load() ? L"正在停止并收尾，请等待线程真正退出…" : L"正在启动/录音；点击“停止并保存”结束。";
            text += L"\r\n已处理音频：" + std::to_wstring(g_job->progress.load() / 48000.0) + L" 秒";
            Status(hwnd, text);
        }
        return;
    }
    if (wait != WAIT_OBJECT_0) {
        Status(hwnd, L"线程等待检查失败；保留会话，不允许重入。Windows错误：" + std::to_wstring(GetLastError()));
        return;
    }
    g_worker.join(); // 已确认线程结束，此处不会长时间阻塞。
    if (g_job->enumerate) {
        g_devices = std::move(g_job->devices);
        SendDlgItemMessageW(hwnd, ID_DEVICE, CB_RESETCONTENT, 0, 0);
        for (const auto& device : g_devices) {
            std::wstring name = Wide(device.description);
            SendDlgItemMessageW(hwnd, ID_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        if (!g_devices.empty()) SendDlgItemMessageW(hwnd, ID_DEVICE, CB_SETCURSEL, 0, 0);
        Status(hwnd, g_job->result.success ? (g_devices.empty() ? L"没有音频设备，请连接设备后刷新。" : L"请选择设备，然后开始录音。")
            : L"设备枚举失败：" + Wide(g_job->result.message));
    } else {
        std::wstring message = Wide(g_job->result.message);
        std::printf("%s; samples=%lld\n", g_job->result.message.c_str(), static_cast<long long>(g_job->result.samples));
        if (g_job->result.success) message = L"保存成功，时长：" + std::to_wstring(g_job->result.samples / 48000.0)
            + L" 秒\r\n" + g_job->options.outputFile;
        else message = (g_job->result.cancelled ? L"已取消：" : L"录音失败：") + message;
        Status(hwnd, message);
    }
    g_job.reset();
    Controls(hwnd);
    if (g_closing) DestroyWindow(hwnd);
}
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        switch (message) {
        case WM_CREATE:
            if (!CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                15, 15, 440, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DEVICE)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"刷新设备", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 465, 15, 110, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"开始录音…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 15, 55, 180, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECORD)), nullptr, nullptr) ||
                !CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 15, 105, 560, 130, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), nullptr, nullptr)) return -1;
            if (!SetTimer(hwnd, TIMER, 50, nullptr)) return -1;
            Refresh(hwnd);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            if (LOWORD(wParam) == ID_REFRESH && !g_job) Refresh(hwnd);
            if (LOWORD(wParam) == ID_RECORD) {
                if (!g_job) StartRecording(hwnd);
                else if (!g_job->enumerate) { g_job->cancel.store(true); Controls(hwnd); }
            }
            return 0;
        case WM_TIMER:
            if (wParam == TIMER) Poll(hwnd);
            return 0;
        case WM_CLOSE:
            if (g_job) { g_closing = true; g_job->cancel.store(true); Controls(hwnd); Status(hwnd, L"正在结束后台任务，完成后自动关闭…"); }
            else DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER);
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    } catch (const std::exception& error) {
        Status(hwnd, L"操作失败：" + Wide(error.what()));
        return message == WM_CREATE ? -1 : 0;
    }
}
int PrintResult(const CaptureResult& result) {
    std::printf("%s; samples=%lld, seconds=%.6f\n", result.message.c_str(),
        static_cast<long long>(result.samples), result.samples / 48000.0);
    return result.success || result.cancelled ? 0 : 1;
}
int CommandLine(int argc, wchar_t** argv) {
    std::wstring command = argv[1];
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
    if (command == L"--convert" && argc == 4) {
        options.inputFile = Utf8(argv[2]); options.outputFile = argv[3];
    } else if ((command == L"--record" || command == L"--cancel-test") && argc == 5) {
        auto devices = ListAudioDevices();
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
            std::thread worker([&] { result = RunAudioCapture(options, cancel, progress); });
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            auto start = std::chrono::steady_clock::now();
            cancel.store(true);
            worker.join();
            std::printf("Stop-to-join: %.3f seconds\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            return PrintResult(result);
        }
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
} // namespace
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc > 1) return CommandLine(argc, argv);
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW window{};
        window.lpfnWndProc = WndProc;
        window.hInstance = instance;
        window.lpszClassName = L"MiniClipAudioCapture";
        window.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&window)) throw std::runtime_error("RegisterClass failed");
        HWND hwnd = CreateWindowW(window.lpszClassName, L"MiniClipStudio — WAV录音", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 620, 300, nullptr, nullptr, instance, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        MSG message{};
        int ret;
        while ((ret = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        // 消息循环异常退出时也不能释放正在使用的Job；真实等待，绝不detach。
        if (g_worker.joinable()) { g_job->cancel.store(true); g_worker.join(); g_job.reset(); }
        if (ret < 0) throw std::runtime_error("GetMessage failed");
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
