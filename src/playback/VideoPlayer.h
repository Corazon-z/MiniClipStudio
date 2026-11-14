// VideoPlayer把S05的两路文件解码、WASAPI主时钟和窗口画面交付组织成一次可控制的播放。
// 控制线程只发请求，Run拥有设备，两个生产线程各自拥有FFmpeg输入；固定队列和单帧邮箱限制提前积压。
#pragma once
#include "PreviewTypes.h"
#include "AudioPlayer.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
namespace miniclip {
struct VideoPlaybackSnapshot {
    PlaybackState state = PlaybackState::Idle;
    uint64_t generation = 0, acknowledgedGeneration = 0;
    int64_t positionUs = 0, requestedUs = 0;
    std::optional<int64_t> durationUs;
    bool audioMaster = false, seeking = false;
    int64_t decodedVideoFrames = 0, scheduledFrames = 0, droppedFrames = 0, paintedFrames = 0;
    int64_t lastPictureUs = -1, maxScheduleLatenessUs = 0, maxPaintLatenessUs = 0;
    int64_t submittedAudioFrames = 0, insertedSilenceFrames = 0, lateAudioFrames = 0;
    uint64_t starvationIntervals = 0;
    size_t videoQueueHighWater = 0, audioQueueHighWater = 0;
    std::string message;
};
class VideoPlayer {
public:
    static constexpr size_t VideoCapacity = 3, AudioCapacity = 8;
    // 窗口或测试先建空闲会话，Start之前不打开输入/设备，也不创建线程。
    VideoPlayer() = default;
    // 最后一个所有者退出前真实停止并回收，不能留下后台访问已失效成员。
    ~VideoPlayer();
    // 线程、设备与队列只有一份管理关系，禁止复制构造。
    VideoPlayer(const VideoPlayer&) = delete;
    // 赋值同样不能复制这组所有权。
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    // GUI的回调仅PostMessage唤醒窗口；没有回调时测试/CLI也可轮询TakePicture。
    void Start(const std::wstring& file, std::function<void()> frameReady = {}, double startSeconds = 0, bool paused = false);
    // 暂停/继续只发布意图，Run在自己的线程冻结设备或单调时钟。
    void SetPaused(bool paused);
    // 新目标立即递增代次并让旧显示失效；自然结束回收后还能在同一文件重新定位。
    uint64_t Seek(double seconds);
    // 关窗/停止先唤醒所有等待，再由Run收回设备与生产者，TryJoin确认真正结束。
    void Stop();
    // 软件音量只改变随后交付的样本，允许0静音和1原音量。
    void SetVolume(float volume);
    // 界面取得锁内复制的状态，不直接访问正在修改的队列或Windows接口。
    VideoPlaybackSnapshot Snapshot() const;
    // 取当前请求最新画面并确认通知；旧代次立即返回空，GUI绘制前还应检查Generation。
    std::shared_ptr<const PreviewPicture> TakePicture();
    // GUI绘制前最后确认请求编号，旧通知不能让上一代画面回流。
    uint64_t Generation() const { return generation_.load(); }
    // GUI绘制成功后反馈实际WM_PAINT的时间观测；与提前发布邮箱的调度指标分开记录。
    void ReportPainted(uint64_t generation, int64_t ptsUs, int64_t durationUs);
    // 零等待观察消费线程，真实退出后才回收，不在GUI上等完整视频。
    bool TryJoin();
    // 只供控制线程判断是否仍有待回收线程；joinable不等于仍在运行。
    bool Active() const { return worker_.joinable(); }
    // 析构和入口异常的兜底仍真实join，不用detach或超时冒充已退出。
    void Shutdown();
private:
    // 协调设备消费、主时钟和画面发布，并在所有退出路径停止/回收两路生产者。
    void Run(std::wstring file);
    // 当前代次的视频输入只由这个生产线程使用，入队前再次核对代次和有限容量。
    void ProduceVideo(PreviewSource source);
    // 独立音轨输入提供带绝对媒体采样位置的PCM，不受视频队列堵塞影响。
    void ProduceAudio(PreviewSource source);
    // 同代次新图替换邮箱，再在锁外合并通知窗口；回调不借出帧内部地址。
    void Publish(std::shared_ptr<const PreviewPicture> picture, int64_t clockUs);
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::shared_ptr<const PreviewPicture>> video_;
    std::deque<PreviewAudioBlock> audio_;
    std::shared_ptr<const PreviewPicture> picture_;
    VideoPlaybackSnapshot snapshot_;
    std::atomic<bool> stop_{false}, noticePending_{false};
    std::atomic<uint64_t> generation_{0};
    std::atomic<float> volume_{0.5f};
    int64_t targetUs_ = 0, videoEndUs_ = 0, audioEndUs_ = 0;
    uint64_t videoDone_ = 0, audioDone_ = 0;
    bool paused_ = false;
    std::string failure_;
    std::wstring lastFile_;
    std::function<void()> frameReady_;
    std::thread worker_;
};
}
