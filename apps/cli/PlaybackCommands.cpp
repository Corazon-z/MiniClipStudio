// 这个文件实现S04的--play命令。RunAudioCommands把播放参数交给这里后，
// 仍使用与窗口相同的AudioPlayer处理声音，只增加控制台按键、Ctrl+C和进度显示。
#include "PlaybackCommands.h"
#include "../../src/playback/AudioPlayer.h"
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <thread>

namespace miniclip {
namespace {
std::atomic<bool> interrupted{false};
// Windows在Ctrl+C/Break事件到来时调用这里，只写原子标志；RunPlaybackCommand的正常循环随后调用Stop。
// 系统回调不直接等待线程或释放设备，避免与播放线程同时操作它的资源。
BOOL WINAPI PlaybackConsoleHandler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true); return TRUE;
}
// 注册资源由本次CLI调用拥有；正常返回或异常展开都会移除回调。
struct ConsoleHandler {
    // RunPlaybackCommand开始播放前创建这个对象，清掉上次请求，再注册本次Ctrl+C/Break回调。
    ConsoleHandler() {
        interrupted.store(false);
        if (!SetConsoleCtrlHandler(PlaybackConsoleHandler, TRUE)) throw std::runtime_error("Register playback Ctrl+C handler failed");
    }
    // 命令正常结束或异常返回时注销回调，让后续任务不再收到本次播放的停止请求。
    ~ConsoleHandler() { SetConsoleCtrlHandler(PlaybackConsoleHandler, FALSE); }
};
}
// RunAudioCommands识别--play后进入这里，读取文件和音量，调用SetVolume/Start启动S04播放。
// 当前CLI线程继续查看P/Q和停止标志，把意图交给AudioPlayer；媒体与设备处理仍在它的后台线程中。
// 每轮TryJoin都不长等，确认线程真正退出后再报告自然完成、主动停止或失败。
int RunPlaybackCommand(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 5) {
        std::puts("Usage: --play INPUT [--volume 0..100]  (P: pause/resume, Q: stop, Ctrl+C: stop)"); return 2;
    }
    float volume = 1.0f;
    if (argc == 5) {
        wchar_t* end = nullptr;
        const long percent = wcstol(argv[4], &end, 10);
        if (std::wstring(argv[3]) != L"--volume" || end == argv[4] || *end || percent < 0 || percent > 100) return 2;
        volume = static_cast<float>(percent) / 100;
    }
    ConsoleHandler handler;
    AudioPlayer player;
    player.SetVolume(volume);
    player.Start(argv[2]);
    std::puts("音频播放：P 暂停/继续；Q 或 Ctrl+C 停止。Windows默认输出设备；不生成新文件。");
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(input, &mode) != FALSE;
    bool paused = false;
    auto lastReport = std::chrono::steady_clock::now();
    while (!player.TryJoin()) {
        if (interrupted.load()) player.Stop();
        // 先问控制台已有多少事件，再用ReadConsoleInputW取这些事件，不等待一个尚未按下的键。
        // P交给SetPaused，Q交给Stop；stdin被重定向时跳过键盘处理，播放仍能自然结束。
        if (console) {
            DWORD count = 0;
            if (!GetNumberOfConsoleInputEvents(input, &count)) throw std::runtime_error("Read console event count failed");
            while (count--) {
                INPUT_RECORD event{}; DWORD read = 0;
                if (!ReadConsoleInputW(input, &event, 1, &read)) throw std::runtime_error("Read playback key failed");
                if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
                const WORD key = event.Event.KeyEvent.wVirtualKeyCode;
                if (key == 'P') { paused = !paused; player.SetPaused(paused); }
                if (key == 'Q') player.Stop();
            }
        }
        const auto now = std::chrono::steady_clock::now();
        // 每秒从Snapshot复制状态显示一次；设备消费时间是估算，不能把已解码数量当成已播放数量。
        if (now - lastReport > std::chrono::seconds(1)) {
            const auto status = player.Snapshot();
            std::printf("%s; consumed~=%.3fs\n", PlaybackStateText(status.state), status.playedFrames / double(PlaybackRate));
            lastReport = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // TryJoin已确认消费线程退出；此后报告终态，0/3/1分别表示自然完成/停止/失败。
    const auto result = player.Snapshot();
    std::printf("%s; decoded=%lld; submitted=%lld; consumed~=%lld; queue_high_water=%zu/%zu; device_buffer=%u; starvation_intervals=%llu\n%s\n",
        PlaybackStateText(result.state), static_cast<long long>(result.decodedFrames), static_cast<long long>(result.submittedFrames),
        static_cast<long long>(result.playedFrames), result.queueHighWater, AudioPlayer::QueueCapacity,
        result.deviceBufferFrames, static_cast<unsigned long long>(result.starvationCount), result.message.c_str());
    return result.state == PlaybackState::Completed ? 0 : result.state == PlaybackState::Stopped ? 3 : 1;
}
}
