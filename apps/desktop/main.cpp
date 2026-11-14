// 这个文件是程序入口和Win32窗口层。无参数时wmain建立窗口，由WndProc响应按钮、定时器和关窗。
// 窗口提供S00录音、S01信息探测、S04音频播放，并可非阻塞打开S05预览窗；耗时工作交各会话管理者，
// 定时器只查看进度并在真实退出后回收线程。S02首帧导出、S03转码通过命令参数进入RunAudioCommands，暂无窗口按钮。
// NOMINMAX防止Windows头文件的宏干扰标准库min/max。
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
#include "../../src/playback/AudioPlayer.h"
#include "../cli/AudioCommands.h"
#include "VideoPreviewWindow.h"
#include "CameraWindow.h"
#include "ConsoleStopSignal.h"
using namespace miniclip;
// Windows打开/保存文件对话框需要的导入库。
#pragma comment(lib, "Comdlg32.lib")
namespace {
constexpr int ID_RECORD = 1001, ID_DEVICE = 1002, ID_REFRESH = 1003, ID_STATUS = 1004, ID_PROBE = 1005;
constexpr int ID_PLAY = 1006, ID_PAUSE = 1007, ID_STOP_PLAY = 1008, ID_VOLUME = 1009;
constexpr int ID_VIDEO = 1010;
constexpr int ID_CAMERA = 1011;
constexpr UINT_PTR TIMER = 1;
// 控制器持有Job唯一所有权，worker借用；GUI在真实退出并join前不能要求清空Job。
using Job = miniclip::RecordingJob;
miniclip::RecordingController g_controller;
miniclip::AudioPlayer g_player;
bool g_pauseRequested = false;
std::vector<AudioDevice> g_devices;
bool g_closing = false;
// Poll、PollPlayback及各处错误显示通过这里，把UTF-8设备名/报告转成Windows控件使用的UTF-16。
// MultiByteToWideChar先确定长度再转换；失败也返回可显示的诊断，避免界面再次抛异常。
std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) return L"无法显示错误文本（无效UTF-8）";
    std::wstring result(n, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), n))
        return L"UTF-8转换失败";
    return result;
}
// 各Start函数、Poll和WndProc把提示或处理结果交给这里，再由SetDlgItemTextW写入状态框。
// 写入前把单独的LF换成Windows多行控件需要的CRLF，保证报告按原来的段落显示。
void Status(HWND hwnd, const std::wstring& text) {
    std::wstring display;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) display += L'\r';
        display += text[i];
    }
    SetDlgItemTextW(hwnd, ID_STATUS, display.c_str());
}
// 开始任务、停止请求和定时轮询都会调用这里，让按钮状态跟随实际任务变化。
// 先用Current/Active/Snapshot查看录音任务与播放状态，再启禁控件、更新按钮文字；
// 当前窗口让录音、探测和播放互斥，避免用户重复启动一份仍未回收的任务。
void Controls(HWND hwnd) {
    const bool taskBusy = static_cast<bool>(g_controller.Current());
    const bool playing = g_player.Active();
    const bool previewing = IsVideoPreviewWindowOpen() || IsCameraWindowOpen();
    const bool busy = taskBusy || playing || previewing || g_closing;
    EnableWindow(GetDlgItem(hwnd, ID_DEVICE), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_REFRESH), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_PROBE), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_RECORD), !g_closing &&
        !playing && !previewing && (taskBusy ? g_controller.Current()->kind == TaskKind::RecordAudio && !g_controller.Current()->cancel.load() : !g_devices.empty()));
    const bool recording = taskBusy && g_controller.Current()->kind == TaskKind::RecordAudio;
    SetDlgItemTextW(hwnd, ID_RECORD, recording ? (g_controller.Current()->cancel.load() ? L"正在停止…" : L"停止并保存") : L"开始录音…");
    EnableWindow(GetDlgItem(hwnd, ID_PLAY), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_VIDEO), !busy);
    EnableWindow(GetDlgItem(hwnd, ID_CAMERA), !busy);
    const auto state = g_player.Snapshot().state;
    const bool controllable = playing && !g_closing && state != PlaybackState::Stopping && !PlaybackFinished(state);
    EnableWindow(GetDlgItem(hwnd, ID_PAUSE), controllable);
    EnableWindow(GetDlgItem(hwnd, ID_STOP_PLAY), controllable);
    SetDlgItemTextW(hwnd, ID_PAUSE, g_pauseRequested ? L"继续播放" : L"暂停播放");
}
// Refresh、StartRecording和StartProbe准备好Job后，经这里移交给RecordingController::Start。
// 控制器接管任务并启动worker；若启动失败就显示错误，随后统一更新按钮。调用前应确认旧任务已回收。
void StartJob(HWND hwnd, std::unique_ptr<Job> job) {
    try { g_controller.Start(std::move(job)); }
    catch (const std::exception& error) { Status(hwnd, L"创建任务失败：" + Wide(error.what())); }
    Controls(hwnd);
}
// 窗口创建或用户点击刷新设备时进入这里，创建EnumerateDevices任务并交给StartJob。
// 查询麦克风可能等待驱动，所以先在后台运行ListAudioDevices，稍后由Poll填入S00设备下拉框。
void Refresh(HWND hwnd) {
    auto job = std::make_unique<Job>();
    job->kind = TaskKind::EnumerateDevices;
    Status(hwnd, L"正在后台枚举麦克风设备…");
    StartJob(hwnd, std::move(job));
}
// WndProc收到开始录音按钮后调用这里，为S00确定麦克风和新WAV路径。
// 先读取设备下拉框，再用GetSaveFileNameW选位置并检查同名文件，最后复制参数、调用StartJob。
// 对话框只选文件名；真正读声音和保存WAV由RecordingController启动的RunAudioCapture完成。
void StartRecording(HWND hwnd) {
    // 组合框可能尚无选择；检查CB_ERR和范围后才取vector元素，防止越界。
    LRESULT index = SendDlgItemMessageW(hwnd, ID_DEVICE, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<size_t>(index) >= g_devices.size()) return;
    // 取当前时间生成建议文件名，再把这块路径缓冲交给同步保存对话框；选好后会复制进Job。
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t path[32768]{};
    swprintf_s(path, L"Recording-%04u%02u%02u-%02u%02u%02u.wav", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    // Win32过滤器按说明/模式使用空字符分隔，末尾双空字符，不能改成普通逗号分隔文本。
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
    // 用户确定路径后才准备录音任务，把设备标识和路径复制进去，避免worker借用即将失效的局部数组。
    job->options.device = g_devices[index].name;
    job->options.outputFile = path;
    Status(hwnd, L"正在打开设备；停止后完成 WAV 保存。实际输入格式见控制台。\r\n输出：48kHz / S16 / 双声道");
    StartJob(hwnd, std::move(job));
}
// WndProc收到查看媒体信息按钮后进入这里，使用GetOpenFileNameW收集S01要探测的文件。
// 确认后把路径装进ProbeMedia任务并调用StartJob；ProbeMediaFile在后台执行，Poll随后显示结果，
// 因此打开/探测文件的等待不会占住当前按钮回调。
void StartProbe(HWND hwnd) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"媒体文件（按内容识别）\0*.*\0\0";
    dialog.lpstrTitle = L"选择要查看信息的媒体文件";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        DWORD error = CommDlgExtendedError();
        if (error) Status(hwnd, L"打开对话框失败：" + std::to_wstring(error));
        return;
    }
    auto job = std::make_unique<Job>();
    job->kind = TaskKind::ProbeMedia;
    job->probeFile = path;
    Status(hwnd, L"正在后台读取媒体信息…可关闭窗口请求取消。");
    StartJob(hwnd, std::move(job));
}
// WndProc收到播放按钮后调用这里，使用GetOpenFileNameW选择S04要听的本地文件。
// AudioPlayer::Start接手路径并创建播放线程，解码和声卡输出由它管理；窗口只刷新按钮，
// 后续进度与退出状态由PollPlayback定时查看，不在这里把文件字节直接写给扬声器。
void StartPlayback(HWND hwnd) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = hwnd;
    dialog.lpstrFile = path; dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrFilter = L"包含音轨的本地媒体文件\0*.*\0\0";
    dialog.lpstrTitle = L"选择要播放的文件（播放第一条音轨）";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        const DWORD error = CommDlgExtendedError();
        if (error) Status(hwnd, L"播放文件对话框失败：" + std::to_wstring(error));
        return;
    }
    g_pauseRequested = false;
    g_player.Start(path); Controls(hwnd);
}
// WM_TIMER定时调用这里观察S04：先用Active/TryJoin检查是否需要回收播放线程，
// 再从Snapshot复制状态交给Status/Controls显示。线程未退出时不长等，继续让窗口响应操作。
// 若用户已经请求关窗，只有TryJoin真正成功且没有另一任务时才DestroyWindow。
void PollPlayback(HWND hwnd) {
    if (!g_player.Active()) return;
    const bool joined = g_player.TryJoin();
    const auto result = g_player.Snapshot();
    std::wstring text = Wide(PlaybackStateText(result.state));
    text += L"\r\n设备已消费（估算）：" + std::to_wstring(result.playedFrames / double(PlaybackRate)) + L" 秒";
    text += L"\r\nPCM队列峰值：" + std::to_wstring(result.queueHighWater) + L" / 8 块；音量只影响本程序。";
    if (!result.message.empty()) text += L"\r\n" + Wide(result.message);
    if (g_closing) text += L"\r\n正在等待播放线程真正退出，随后关闭窗口。";
    Status(hwnd, text); Controls(hwnd);
    if (joined && g_closing && !g_controller.Current() && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) DestroyWindow(hwnd);
}
// WM_TIMER调用这里查看RecordingController管理的S00录音/枚举和S01探测任务。
// TryJoin未完成时只读原子进度并返回；成功后才按TaskKind更新设备、探测报告或保存结果，
// 然后ClearFinished归还Job、恢复按钮，并在此前收到关窗请求时真正销毁窗口。
void Poll(HWND hwnd) {
    if (!g_controller.Current()) return;
    if (!g_controller.TryJoin()) {
        if (g_controller.Current()->kind == TaskKind::RecordAudio) {
            std::wstring text = g_controller.Current()->cancel.load() ? L"正在停止并收尾，请等待线程真正退出…" : L"正在启动/录音；点击“停止并保存”结束。";
            text += L"\r\n已处理音频：" + std::to_wstring(g_controller.Current()->progress.load() / 48000.0) + L" 秒";
            Status(hwnd, text);
        }
        return;
    }
    // TryJoin成功已建立同步；下面才可读取worker写入的普通结果和设备数组。
    if (g_controller.Current()->kind == TaskKind::EnumerateDevices) {
        // 后台已把设备名称复制成自有字符串；这里接过清单、重建S00设备组合框，控件始终由GUI操作。
        g_devices = std::move(g_controller.Current()->devices);
        SendDlgItemMessageW(hwnd, ID_DEVICE, CB_RESETCONTENT, 0, 0);
        for (const auto& device : g_devices) {
            std::wstring name = Wide(device.description);
            SendDlgItemMessageW(hwnd, ID_DEVICE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        if (!g_devices.empty()) SendDlgItemMessageW(hwnd, ID_DEVICE, CB_SETCURSEL, 0, 0);
        Status(hwnd, g_controller.Current()->result.success ? (g_devices.empty() ? L"没有音频设备，请连接设备后刷新。" : L"请选择设备，然后开始录音。")
            : L"设备枚举失败：" + Wide(g_controller.Current()->result.message));
    } else if (g_controller.Current()->kind == TaskKind::ProbeMedia) {
        // S01后台任务已关闭输入并留下独立信息单；FormatMediaInfo排版后，经Wide转码交Status显示。
        if (g_controller.Current()->result.success) {
            const std::string report = FormatMediaInfo(g_controller.Current()->mediaInfo);
            std::printf("%s", report.c_str());
            Status(hwnd, Wide(report));
        } else {
            Status(hwnd, L"媒体探测未完成：" + Wide(g_controller.Current()->result.message));
        }
    } else {
        // S00只有在WAV发布成功且worker退出后才显示保存成功，并用实际样本数换算时长。
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
    if (g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) DestroyWindow(hwnd);
}
// wmain把这里登记为窗口过程，Windows随后把建窗、按钮、定时器和关闭消息交给它。
// WM_COMMAND调用各Start函数或只发暂停/停止请求，WM_TIMER调用两个Poll查看结果，
// WM_CLOSE先请求结束任务再延后关窗；耗时媒体工作不在这里执行，异常也不能逃出Windows回调边界。
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    try {
        switch (message) {
        // CreateWindowW建主窗口时会发来WM_CREATE；这里建立S00/S01/S04控件、设置音量和定时器，
        // 再用Refresh后台枚举麦克风。任一控件创建失败就终止建窗，不留下半套可操作界面。
        case WM_CREATE:
            if (!CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                15, 15, 440, 200, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DEVICE)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"刷新设备", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 465, 15, 110, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"开始录音…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 15, 55, 180, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RECORD)), nullptr, nullptr) ||
                !CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 15, 155, 560, 315, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)), nullptr, nullptr)) return -1;
            if (!CreateWindowW(L"BUTTON", L"查看媒体信息…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                210, 55, 180, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PROBE)), nullptr, nullptr)) return -1;
            if (!CreateWindowW(L"BUTTON", L"打开视频…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                405, 55, 170, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIDEO)), nullptr, nullptr)) return -1;
            if (!CreateWindowW(L"BUTTON", L"拍摄 / 区域录屏…", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                15, 480, 180, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CAMERA)), nullptr, nullptr)) return -1;
            if (!CreateWindowW(L"BUTTON", L"播放音频…", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 15, 105, 150, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PLAY)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"暂停播放", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 175, 105, 110, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PAUSE)), nullptr, nullptr) ||
                !CreateWindowW(L"BUTTON", L"停止播放", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 295, 105, 110, 35, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STOP_PLAY)), nullptr, nullptr) ||
                !CreateWindowW(L"STATIC", L"音量", WS_CHILD | WS_VISIBLE, 415, 113, 45, 25, hwnd, nullptr, nullptr, nullptr) ||
                !CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 465, 109, 110, 180, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VOLUME)), nullptr, nullptr)) return -1;
            for (const auto* label : { L"0%（静音）", L"25%", L"50%", L"75%", L"100%" })
                SendDlgItemMessageW(hwnd, ID_VOLUME, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendDlgItemMessageW(hwnd, ID_VOLUME, CB_SETCURSEL, 2, 0);
            g_player.SetVolume(0.5f);
            // 50ms定时消息由GUI循环处理，不会创建新线程，也不是声卡的实时消费时钟。
            if (!SetTimer(hwnd, TIMER, 50, nullptr)) return -1;
            Refresh(hwnd);
            return 0;
        // 音量组合框发CBN_SELCHANGE，按钮发BN_CLICKED；先辨别通知，再检查任务能否启动。
        // 录音/探测交StartJob，播放交AudioPlayer；停止和暂停只写请求，不在GUI释放worker仍使用的资源。
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_VOLUME && HIWORD(wParam) == CBN_SELCHANGE) {
                const LRESULT selected = SendDlgItemMessageW(hwnd, ID_VOLUME, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected <= 4) g_player.SetVolume(static_cast<float>(selected) / 4);
                return 0;
            }
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            if (LOWORD(wParam) == ID_CAMERA && !g_controller.Current() && !g_player.Active() && !g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) {
                OpenCameraWindow(hwnd); Controls(hwnd);
            }
            if (LOWORD(wParam) == ID_VIDEO && !g_controller.Current() && !g_player.Active() && !g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) {
                OpenVideoPreviewWindow(hwnd); Controls(hwnd);
            }
            if (LOWORD(wParam) == ID_PLAY && !g_controller.Current() && !g_player.Active() && !g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) StartPlayback(hwnd);
            if (LOWORD(wParam) == ID_PAUSE && g_player.Active()) {
                g_pauseRequested = !g_pauseRequested; g_player.SetPaused(g_pauseRequested); Controls(hwnd);
            }
            if (LOWORD(wParam) == ID_STOP_PLAY) { g_player.Stop(); Controls(hwnd); }
            if (LOWORD(wParam) == ID_REFRESH && !g_controller.Current() && !g_player.Active() && !g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) Refresh(hwnd);
            if (LOWORD(wParam) == ID_PROBE && !g_controller.Current() && !g_player.Active() && !g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) StartProbe(hwnd);
            if (LOWORD(wParam) == ID_RECORD && !g_player.Active() && !g_closing && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) {
                if (!g_controller.Current()) StartRecording(hwnd);
                else if (g_controller.Current()->kind == TaskKind::RecordAudio) { g_controller.Current()->cancel.store(true); Controls(hwnd); }
            }
            return 0;
        // 定时消息仍在GUI线程处理；分别查看S04播放与S00/S01任务，两处都要确认真实退出才回收。
        case WM_TIMER:
            if (wParam == TIMER) {
                if (ConsoleStopSignal::TakeRequest()) {
                    // 与点主窗×相同：子会话先Stop，真正保存/join后才退出消息循环。
                    SendMessageW(hwnd, WM_CLOSE, 0, 0);
                    return 0; // WM_CLOSE可能已销毁空闲窗口，不再访问其控件。
                }
                PollPlayback(hwnd); Poll(hwnd);
                if (g_closing && !g_controller.Current() && !g_player.Active() && !IsVideoPreviewWindowOpen() && !IsCameraWindowOpen()) DestroyWindow(hwnd);
                else Controls(hwnd);
            }
            return 0;
        // 关窗先记住g_closing并请求停止，保留消息循环让后续Poll继续查看；不能先删掉任务仍在用的资源。
        case WM_CLOSE:
            if (g_controller.Current() || g_player.Active() || IsVideoPreviewWindowOpen() || IsCameraWindowOpen()) {
                g_closing = true;
                if (g_controller.Current()) g_controller.Current()->cancel.store(true);
                RequestVideoPreviewClose();
                RequestCameraClose();
                g_player.Stop(); Controls(hwnd); Status(hwnd, L"正在结束后台任务，完成后自动关闭…");
            }
            else DestroyWindow(hwnd);
            return 0;
        // 窗口真正销毁后停止定时器，再PostQuitMessage使wmain退出消息循环。
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
} // namespace
// 程序从这里开始：有命令参数时调用RunAudioCommands，S02/S03也从这条CLI路径进入。
// 无参数时先RegisterClassW登记WndProc，再CreateWindowW建窗，进入GetMessage/DispatchMessageW消息循环，
// 让Windows驱动S00/S01/S04按钮和S05独立预览；退出时各会话真实停止并回收线程。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        if (argc > 1) {
            const std::wstring command = argv[1];
            if (command == L"--camera" || command == L"--preview") {
                ConsoleStopSignal consoleStop;
                return RunAudioCommands(argc, argv);
            }
            return RunAudioCommands(argc, argv);
        }
        ConsoleStopSignal consoleStop; // 保持到全部后台任务收尾、消息循环退出之后。
        HINSTANCE instance = GetModuleHandleW(nullptr);
        // 先填写窗口类，把WndProc地址交给Windows；登记地址本身不执行回调，消息到来时才调用它。
        WNDCLASSW window{};
        window.lpfnWndProc = WndProc;
        window.hInstance = instance;
        window.lpszClassName = L"MiniClipAudioCapture";
        window.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&window)) throw std::runtime_error("RegisterClass failed");
        // CreateWindowW会在创建期间调用WndProc处理WM_CREATE，控件都建好后再用ShowWindow显示。
        HWND hwnd = CreateWindowW(window.lpszClassName, L"MiniClipStudio — 录音、播放与拍摄", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 620, 570, nullptr, nullptr, instance, nullptr);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        MSG message{};
        int ret;
        // 消息循环：正数处理，0正常退出，-1是错误。IsDialogMessage支持控件键盘导航。
        while ((ret = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!ProcessCameraMessage(message) && !ProcessVideoPreviewMessage(message) && !IsDialogMessageW(hwnd, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        // 退出兜底：两个Shutdown先请求停止再真实join，异常退出也不能detach或提前销毁共享资源。
        g_controller.Shutdown();
        g_player.Shutdown();
        // 消息循环异常结束也要先销毁预览，由窗口数据的VideoPlayer析构真实Stop/join兜底。
        RequestVideoPreviewClose();
        RequestCameraClose();
        if (IsWindow(hwnd)) DestroyWindow(hwnd);
        if (ret < 0) throw std::runtime_error("GetMessage failed");
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
