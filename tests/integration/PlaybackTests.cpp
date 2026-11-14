// 这个文件检查S04的AudioPlayer能否完整播放、暂停后继续，以及停止后再次启动。
// DecodePlaybackFixture调用实际解码模块输出PCM数值，TestPlaybackDevice则使用真实默认输出设备。
// 设备测试将音量置0，仍检查真实消费进度；音质和左右声道的听感还需要用户试听。
#include "PlaybackTests.h"
#include "../../src/playback/AudioPlayer.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
using namespace miniclip;
namespace {
// 播放测试用这个函数检查条件；不满足时立即抛出带原因的异常，由测试入口报告失败。
void RequirePlayback(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
// 反复调用传入的判断函数，直到目标状态出现；超过15秒就报告测试等待失败。
// steady_clock让日历调时不影响期限；这个期限只管测试等待，不会强行终止后台线程。
template<class Predicate> void WaitFor(Predicate ready, const char* text) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!ready()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(text);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
// 通过WaitFor不断调用AudioPlayer::TryJoin，确认消费线程真正退出并回收后才继续检查。
// 即使测试超时，AudioPlayer析构仍负责请求停止和真实join，不能用超时代替线程结束。
void JoinPlayback(AudioPlayer& player) {
    WaitFor([&] { return player.TryJoin(); }, "Playback thread did not exit within test deadline");
}
}
// 先打开一个新的测试输出文件，再调用DecodeAudioFile，把回调中的有效样本逐块写出。
// 写完关闭文件后，再以预取消和不存在的路径调用解码入口，检查取消是否优先于文件访问。
void DecodePlaybackFixture(const std::wstring& input, const std::wstring& output) {
    if (std::filesystem::exists(output)) throw std::runtime_error("Test PCM output already exists");
    std::ofstream file(std::filesystem::path(output), std::ios::binary);
    if (!file) throw std::runtime_error("Cannot create test PCM output");
    std::atomic<bool> stop{false};
    // DecodeAudioFile在当前线程交来每个PCM块时，回调只写frames指定的有效样本，不写数组剩余容量。
    const auto result = DecodeAudioFile(input, stop, [&](PcmBlock block) {
        RequirePlayback(block.frames > 0 && block.frames <= PcmBlockFrames, "Unbounded PCM block");
        file.write(reinterpret_cast<const char*>(block.samples.data()), block.frames * PlaybackChannels * sizeof(float));
        if (!file) throw std::runtime_error("Write test PCM failed");
    });
    file.close();
    if (!file) throw std::runtime_error("Close test PCM failed");
    std::printf("DECODE frames=%lld stream=%d rate=%d channels=%d\n", static_cast<long long>(result.frames), result.streamIndex, PlaybackRate, PlaybackChannels);
    // 已停止的任务必须在查路径/分配解码器之前返回；不存在的输入也不应盖过明确停止。
    stop.store(true);
    bool cancelled = false;
    try { DecodeAudioFile(L"does-not-exist.wav", stop, [](PcmBlock) {}); }
    catch (const PlaybackCancelled&) { cancelled = true; }
    RequirePlayback(cancelled, "Decoder pre-cancel not honored");
}
// 这个函数实际启动S04的AudioPlayer，观察设备消费进度，检查暂停恢复、完整结束和重复启停。
// 下面按启动→暂停冻结→恢复完成→满队列停止→重启/失败恢复的顺序验证，声音增益设0以保持静音。
void TestPlaybackDevice(const std::wstring& input) {
    AudioPlayer player;
    player.SetVolume(0); // 真实设备消耗真实样本，但自动检查不发出测试音。
    bool badVolume = false;
    try { player.SetVolume(std::numeric_limits<float>::quiet_NaN()); } catch (const std::invalid_argument&) { badVolume = true; }
    RequirePlayback(badVolume, "NaN volume accepted");
    player.Start(input);
    bool reentry = false;
    try { player.Start(input); } catch (const std::logic_error&) { reentry = true; }
    RequirePlayback(reentry, "Concurrent playback restart accepted");
    // 传给WaitFor的判断先检查播放失败，再等设备至少消费4800帧；确认已开播后才开始暂停测试。
    WaitFor([&] {
        const auto state = player.Snapshot();
        if (state.state == PlaybackState::Failed) throw std::runtime_error(state.message);
        return state.playedFrames >= 4800;
    }, "Output device did not consume audio");
    // 先发暂停请求，等消费线程确认Paused，再间隔180ms比较快照，检查提交和消费计数是否保持不变。
    player.SetPaused(true);
    WaitFor([&] { return player.Snapshot().state == PlaybackState::Paused; }, "Pause not acknowledged");
    const auto paused = player.Snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    const auto stillPaused = player.Snapshot();
    RequirePlayback(paused.playedFrames == stillPaused.playedFrames && paused.submittedFrames == stillPaused.submittedFrames,
        "Paused playback advanced");
    player.SetVolume(0.25f); player.SetVolume(0); // 验证可变音量接口；听感与幅度另行人工/PCM检查。
    // 恢复后先等已消费帧数增长，再Join到自然结束，最后比较解码、提交和消费总数是否一致。
    player.SetPaused(false);
    WaitFor([&] { return player.Snapshot().playedFrames > paused.playedFrames; }, "Resume did not advance");
    JoinPlayback(player);
    const auto complete = player.Snapshot();
    RequirePlayback(complete.state == PlaybackState::Completed, complete.message.c_str());
    RequirePlayback(complete.decodedFrames == complete.submittedFrames && complete.playedFrames == complete.submittedFrames, "Playback tail was lost");
    RequirePlayback(complete.queueHighWater <= AudioPlayer::QueueCapacity, "Queue capacity exceeded");
    std::printf("DEVICE completed frames=%lld queue_high_water=%zu device_buffer=%u starvation_intervals=%llu\n",
        static_cast<long long>(complete.playedFrames), complete.queueHighWater, complete.deviceBufferFrames,
        static_cast<unsigned long long>(complete.starvationCount));
    // 让队列先达到8块上限，再暂停并Stop；这样能检查正在等空位的生产线程是否也会被唤醒退出。
    player.Start(input);
    WaitFor([&] { return player.Snapshot().queueHighWater == AudioPlayer::QueueCapacity; }, "Producer never reached bounded capacity");
    player.SetPaused(true);
    WaitFor([&] { return player.Snapshot().state == PlaybackState::Paused; }, "Second pause failed");
    const auto start = std::chrono::steady_clock::now();
    player.Stop(); JoinPlayback(player);
    RequirePlayback(player.Snapshot().state == PlaybackState::Stopped, "Stop while paused failed");
    const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // 这里连续执行启动、停止和join，再尝试不存在的输入，检查同一对象能重置且失败后不遗留活动线程。
    for (int i = 0; i < 3; ++i) {
        player.Start(input); player.Stop(); JoinPlayback(player);
        RequirePlayback(player.Snapshot().state == PlaybackState::Stopped, "Rapid stop/restart failed");
    }
    player.Start(L"does-not-exist.wav"); JoinPlayback(player);
    RequirePlayback(player.Snapshot().state == PlaybackState::Failed, "Missing input did not fail");
    // 异常后再次打开同一真实设备验证没有遗留独占/活线程。
    player.Start(input); player.Stop(); JoinPlayback(player);
    std::printf("PASS playback pause/resume/tail/bounded queue/stop/restart/failure; stop_ms=%.3f\n", stopMs);
}
