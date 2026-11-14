// AudioPlayer是S04播放功能的项目会话管理器，窗口和--play入口都通过它控制同一条播放链路。
// Start安排Run打开WASAPI，再启动Produce调用DecodeAudioFile准备PCM；Run按设备速度取走，有限队列防止无限提前堆积。
// Start/TryJoin/Shutdown/Active由同一GUI或CLI控制线程调用；其余控制与快照接口通过锁/atomic允许和后台并发。
#pragma once
#include "../media/audio/AudioDecode.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace miniclip {
enum class PlaybackState { Idle, Opening, Playing, Paused, Stopping, Completed, Stopped, Failed };
struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::Idle;
    int64_t decodedFrames = 0;
    int64_t submittedFrames = 0;
    int64_t playedFrames = 0; // submitted - device padding，应用格式下的消费估算；不是精确硬件呈现时钟。
    size_t queueHighWater = 0;
    unsigned deviceBufferFrames = 0;
    uint64_t starvationCount = 0; // 已启动且无设备数据、无队列数据时的连续饥饿区间数。
    std::string message;
};
// 控制界面用它判断是否已完成、已停止或失败；Stopping只是正在收尾，仍不能当作结束。
bool PlaybackFinished(PlaybackState state);
// GUI/CLI用它取得状态对应的中文UTF-8文字，返回静态字符串，调用方无需释放。
const char* PlaybackStateText(PlaybackState state);
class AudioPlayer {
public:
    static constexpr size_t QueueCapacity = 8;
    // GUI或CLI先创建空闲会话，直到Start才启动后台并打开设备。
    AudioPlayer() = default;
    // 对象离开作用域前先Shutdown，确保借用它的线程已退出，不能留下后台访问失效对象。
    ~AudioPlayer();
    // 线程、锁和队列只归一个会话管理，不能复制成两份所有权。
    AudioPlayer(const AudioPlayer&) = delete;
    // 赋值也不能复制这组线程和共享状态。
    AudioPlayer& operator=(const AudioPlayer&) = delete;
    // 窗口选文件或CLI解析完参数后调用Start，复位会话并创建执行Run的消费线程。
    // 上一次即使已播完，也必须先TryJoin回收，不能让两次播放同时使用同一组状态。
    void Start(const std::wstring& file);
    // 暂停/继续按钮只在这里发布意图并通知后台，真正的WASAPI暂停/启动由消费线程处理。
    void SetPaused(bool paused);
    // 停止按钮或关窗请求通过这里置停止标志、唤醒等待者；实际资源回收完成后才进入Stopped。
    void Stop();
    // 音量控件或CLI发布0—1增益，0静音、1原音量；只影响新交给设备的样本，不改系统总音量。
    void SetVolume(float volume);
    // GUI定时器和CLI读取一份加锁复制的状态，避免直接访问仍在变化的内部队列与计数。
    PlaybackSnapshot Snapshot() const;
    // GUI/CLI轮询线程是否真实退出；未退出立刻返回，已退出才join回收，不让窗口长时间等待。
    bool TryJoin();
    // 供控制线程判断是否还有待回收的播放线程；true可能只是已退出但尚未join，并不保证仍在运行。
    bool Active() const { return worker_.joinable(); }
    // 入口退出或析构时先Stop再真实join兜底，不能detach或在超时后假装线程已经结束。
    void Shutdown();
private:
    // Start创建的消费线程从Run进入，打开WASAPI、启动生产线程，按设备空位写样本并统一收尾。
    void Run(std::wstring file);
    // Run创建的生产线程从Produce进入，调用DecodeAudioFile准备声音；队列满或暂停时等待，不能无限提前解码。
    void Produce(const std::wstring& file);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<PcmBlock> queue_;
    PlaybackSnapshot snapshot_;
    bool paused_ = false;
    bool producerDone_ = false;
    std::string producerError_;
    std::atomic<bool> stop_{false};
    std::atomic<float> volume_{1.0f};
    std::thread worker_;
};
}
