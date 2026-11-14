// 窗口模式用这个小工具接住控制台Ctrl+C/Ctrl+Break，再由GUI走正常关窗和保存流程。
// Windows会另开线程执行控制台回调；回调只写atomic，不碰窗口、会话或FFmpeg资源。
#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace miniclip {
class ConsoleStopSignal {
public:
    // wmain仅为一个GUI入口登记处理器；CLI录制仍使用自己的“停止并保存”处理器。
    ConsoleStopSignal() {
        requested_.store(false);
        if (!SetConsoleCtrlHandler(OnConsole, TRUE))
            throw std::runtime_error("Install GUI console stop handler failed");
    }
    // 生命周期覆盖消息循环与后台收尾；静态原子变量不会因迟到回调而悬空。
    ~ConsoleStopSignal() {
        if (!SetConsoleCtrlHandler(OnConsole, FALSE))
            std::fputs("Remove GUI console stop handler failed\n", stderr);
    }
    ConsoleStopSignal(const ConsoleStopSignal&) = delete;
    ConsoleStopSignal& operator=(const ConsoleStopSignal&) = delete;

    // 只有顶层GUI定时器取走请求，随后投递WM_CLOSE；子窗口不抢走主窗的退出请求。
    static bool TakeRequest() { return requested_.exchange(false); }
private:
    // 返回TRUE表示本次Ctrl+C已处理，阻止Windows默认直接结束进程。
    // 关闭控制台/强制结束进程有不同契约，不能在这里声称也能可靠保存。
    static BOOL WINAPI OnConsole(DWORD event) {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
        requested_.store(true);
        return TRUE;
    }
    static inline std::atomic<bool> requested_{false};
};
}
