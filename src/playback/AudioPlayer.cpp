// 这里把S04的解码与设备输出连接成可控制的播放会话。窗口或CLI调用Start后，Run打开WASAPI，
// 再启动Produce用FFmpeg准备PCM；中间最多8块的队列让生产速度服从设备的消费速度。
// 解码完成不等于播放完成，Run还需等设备缓冲播完；暂停、停止和错误也都在这里协调两个线程收尾。
#define NOMINMAX
#include "AudioPlayer.h"
#include "../platform/windows/output/WasapiOutput.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace miniclip {
// GUI/CLI用这个判断是否进入完成、停止或失败终态；Stopping仍在收尾，不能提前回收共享资源。
bool PlaybackFinished(PlaybackState state) {
    return state == PlaybackState::Completed || state == PlaybackState::Stopped || state == PlaybackState::Failed;
}
// 把播放状态转换为GUI和CLI使用的中文文字，返回静态字符串，不分配或转移字符串所有权。
const char* PlaybackStateText(PlaybackState state) {
    switch (state) {
    case PlaybackState::Idle: return "空闲";
    case PlaybackState::Opening: return "正在打开";
    case PlaybackState::Playing: return "正在播放";
    case PlaybackState::Paused: return "已暂停";
    case PlaybackState::Stopping: return "正在停止";
    case PlaybackState::Completed: return "播放结束";
    case PlaybackState::Stopped: return "已停止";
    case PlaybackState::Failed: return "播放失败";
    }
    return "未知";
}
// 会话对象销毁前先Shutdown，确保消费线程及其拥有的生产线程都不再借用本对象。
AudioPlayer::~AudioPlayer() { Shutdown(); }
// 窗口和CLI开始播放时先拒绝未回收的旧线程，在锁内清队列/复位状态，再用std::thread启动Run。
// 文件路径被复制进新线程，不借用选文件对话框即将失效的局部数组；创建线程失败则记录Failed并向上抛出。
void AudioPlayer::Start(const std::wstring& file) {
    if (worker_.joinable()) throw std::logic_error("Join previous playback before restarting");
    {
        std::lock_guard lock(mutex_);
        queue_.clear(); snapshot_ = {}; snapshot_.state = PlaybackState::Opening;
        paused_ = false; producerDone_ = false; producerError_.clear(); stop_.store(false);
    }
    try { worker_ = std::thread(&AudioPlayer::Run, this, file); }
    catch (...) { std::lock_guard lock(mutex_); snapshot_.state = PlaybackState::Failed; throw; }
}
// 暂停按钮或CLI的P键在锁内修改暂停意图，再notify_all让生产/消费线程重新检查条件。
// 控制线程不直接操作声卡；Run观察意图后才调用WasapiOutput::Pause或Start。
void AudioPlayer::SetPaused(bool paused) {
    {
        std::lock_guard lock(mutex_);
        if (PlaybackFinished(snapshot_.state) || snapshot_.state == PlaybackState::Idle || stop_.load()) return;
        paused_ = paused;
    }
    changed_.notify_all();
}
// 停止按钮、关窗和CLI取消共用这条路径：在同一mutex内置停止标志和Stopping，再唤醒等待者。
// 满队列时生产者可能等空位，暂停时消费者也可能休眠，两端都得被叫醒后才有机会真正退出并join。
void AudioPlayer::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (snapshot_.state == PlaybackState::Idle || PlaybackFinished(snapshot_.state)) return;
        stop_.store(true);
        snapshot_.state = PlaybackState::Stopping;
    }
    // 无论生产者等“队列有空位”还是消费者等“继续”，停止都必须把它们唤醒。
    changed_.notify_all();
}
// 音量控件/CLI交来的增益先排除NaN、无穷大和越界值，再原子更新，供Run下次写样本时读取。
void AudioPlayer::SetVolume(float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1) throw std::invalid_argument("Playback volume must be 0..1");
    volume_.store(volume);
}
// GUI定时器或CLI取状态时持锁复制，防止同时读到更新前后的字段而得到互相矛盾的数字。
PlaybackSnapshot AudioPlayer::Snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
// GUI/CLI轮询时先用WaitForSingleObject做零等待检查，真实退出后才join；没退出就立即返回。
// joinable只表示std::thread仍有待回收的管理关系，不能单凭它判断线程是否已经运行完。
bool AudioPlayer::TryJoin() {
    if (!worker_.joinable()) return false;
    const DWORD result = WaitForSingleObject(worker_.native_handle(), 0);
    if (result == WAIT_TIMEOUT) return false;
    if (result != WAIT_OBJECT_0) throw std::runtime_error("Cannot inspect playback thread");
    worker_.join();
    return true;
}
// 入口退出和析构以Stop加真实join兜底，等待仍须回收的线程；不能detach后把资源留给仍在执行的后台。
void AudioPlayer::Shutdown() { Stop(); if (worker_.joinable()) worker_.join(); }
// Run启动Produce后，它在生产线程同步调用DecodeAudioFile；解码回调等待队列有空位，再交出独立PCM块。
// 正常结束、用户停止或解码失败都要记录producerDone并通知Run，失败原因另存，不能把所有异常都当作取消。
void AudioPlayer::Produce(const std::wstring& file) {
    try {
        // DecodeAudioFile每交一块就进入这个回调；等未暂停且有空位后入队，更新帧数/峰值，再通知消费者。
        DecodeAudioFile(file, stop_, [&](PcmBlock block) {
            std::unique_lock lock(mutex_);
            // 停止也必须能打破等待，否则消费者已停，生产者却可能永远等不到新的空位。
            changed_.wait(lock, [&] { return stop_.load() || (!paused_ && queue_.size() < QueueCapacity); });
            if (stop_.load()) throw PlaybackCancelled();
            snapshot_.decodedFrames += block.frames;
            queue_.push_back(std::move(block));
            snapshot_.queueHighWater = std::max(snapshot_.queueHighWater, queue_.size());
            lock.unlock();
            changed_.notify_all();
        });
    } catch (const PlaybackCancelled&) {
        // 明确的协作式停止；真正解码异常保留为失败，不能因同时停止就吞掉错误。
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_); producerError_ = error.what();
    } catch (...) {
        std::lock_guard lock(mutex_); producerError_ = "Unknown audio decoder failure";
    }
    { std::lock_guard lock(mutex_); producerDone_ = true; }
    changed_.notify_all();
}
// Start把Run安排到消费线程；这里先打开WasapiOutput，再创建Produce，让解码与设备消费通过有限队列衔接。
// 每轮读padding计算空位，取块后Write并Start/Wait；自然结束必须等设备也空，停止或失败则丢弃待播内容。
// WASAPI只在本线程使用，生产线程也由这里真实join，最后才发布可由控制线程观察的终态。
void AudioPlayer::Run(std::wstring file) {
    std::thread producer;
    PlaybackState finalState = PlaybackState::Completed;
    std::string error;
    try {
        // 设备与COM都在消费者线程内创建、使用和销毁；GUI只发送请求。
        WasapiOutput output;
        if (stop_.load()) throw PlaybackCancelled();
        output.Open();
        { std::lock_guard lock(mutex_); snapshot_.deviceBufferFrames = output.Capacity(); }
        producer = std::thread(&AudioPlayer::Produce, this, std::cref(file));
        PcmBlock current;
        int offset = 0;
        bool deviceStarted = false;
        bool starving = false;
        auto lastAdvance = std::chrono::steady_clock::now();
        int64_t lastPlayed = 0;
        // 先读设备消费量，再持短锁检查生产错误、暂停和队列；调用设备API时不占共享锁。
        for (;;) {
            if (stop_.load()) throw PlaybackCancelled();
            const UINT32 padding = output.Padding();
            {
                std::unique_lock lock(mutex_);
                snapshot_.playedFrames = snapshot_.submittedFrames - padding;
                if (!producerError_.empty()) throw std::runtime_error(producerError_);
                if (paused_) {
                    // 不持有共享锁调用设备API，GUI的Snapshot/Stop不必等驱动操作。
                    // 此处IAudioClient::Stop暂停消费并保留待播缓冲；继续不会从头解码。
                    lock.unlock();
                    output.Pause(); deviceStarted = false;
                    const UINT32 pausedPadding = output.Padding();
                    lock.lock();
                    if (stop_.load()) throw PlaybackCancelled();
                    snapshot_.playedFrames = snapshot_.submittedFrames - pausedPadding;
                    snapshot_.state = PlaybackState::Paused;
                    // 暂停保留当前块和设备缓冲；继续、停止或生产错误都能唤醒，不能只等“继续”。
                    changed_.wait(lock, [&] { return stop_.load() || !paused_ || !producerError_.empty(); });
                    lastAdvance = std::chrono::steady_clock::now();
                    continue;
                }
                if (stop_.load()) throw PlaybackCancelled();
                snapshot_.state = PlaybackState::Playing;
                if (offset == current.frames && !queue_.empty()) {
                    current = std::move(queue_.front()); queue_.pop_front(); offset = 0;
                    changed_.notify_all();
                }
                // 自然结束需四项同时满足：生产结束、当前块用完、队列空、设备也已消费完。
                if (offset == current.frames && queue_.empty() && producerDone_ && padding == 0) break;
                const bool empty = deviceStarted && padding == 0 && offset == current.frames && queue_.empty() && !producerDone_;
                if (empty && !starving) ++snapshot_.starvationCount;
                starving = empty;
                if (!deviceStarted && offset == current.frames && padding == 0) {
                    // 首块尚未到就释放mutex等待数据、完成或控制请求，别在空队列上持续空转。
                    changed_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                        return stop_.load() || paused_ || producerDone_ || !queue_.empty();
                    });
                    continue;
                }
            }
            // 当前块可能比设备空位大，只交空位能容纳的部分；offset记住余下位置，下轮继续。
            const UINT32 available = output.Capacity() - padding;
            if (available && offset < current.frames) {
                const UINT32 count = std::min(available, static_cast<UINT32>(current.frames - offset));
                output.Write(current.samples.data() + offset * PlaybackChannels, count, volume_.load());
                offset += count;
                std::lock_guard lock(mutex_); snapshot_.submittedFrames += count;
            }
            if (!deviceStarted) { output.Start(); deviceStarted = true; }
            // 当前块已写完或设备无空位时等待事件/短超时，让出CPU并在下轮重新读取padding。
            if (available == 0 || offset == current.frames) output.Wait();
            const auto now = std::chrono::steady_clock::now();
            const int64_t played = Snapshot().playedFrames;
            if (played != lastPlayed) { lastPlayed = played; lastAdvance = now; }
            if (now - lastAdvance > std::chrono::seconds(10)) throw std::runtime_error("Playback made no progress for 10 seconds");
        }
        output.Pause(); // 自然结束必须先等padding=0；停止/异常则直接销毁并丢弃待播缓冲。
    } catch (const PlaybackCancelled&) { finalState = PlaybackState::Stopped; }
    catch (const std::exception& failure) { finalState = PlaybackState::Failed; error = failure.what(); }
    catch (...) { finalState = PlaybackState::Failed; error = "Unknown playback failure"; }
    // 消费端失败也要通知生产者退出，否则它可能永远堵在已满的队列上。
    // 条件变量的判定状态也必须在同一mutex下修改：即使stop是atomic，
    // 不加锁仍可能在“检查条件→真正睡眠”的间隙通知，导致漏唤醒。
    { std::lock_guard lock(mutex_); stop_.store(true); }
    changed_.notify_all();
    if (producer.joinable()) producer.join();
    {
        std::lock_guard lock(mutex_);
        if (!producerError_.empty()) { finalState = PlaybackState::Failed; error = producerError_; }
        queue_.clear(); snapshot_.state = finalState; snapshot_.message = error;
    }
}
}
