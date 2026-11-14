// 这个文件定义窗口使用的后台任务管理者，负责S00录音/设备枚举和S01媒体探测。
// Start接过任务并创建worker，窗口定时调用TryJoin，确认结束后再读取结果、ClearFinished。
// 控制器独占Job，worker只借用；运行中只并发访问约定的atomic字段。S04由AudioPlayer另行管理。
#pragma once
#include "../capture/audio/AudioRecorder.h"
#include "../media/probe/MediaProbe.h"
#include <memory>
#include <thread>
namespace miniclip {
// worker根据这个枚举选择录音、枚举或探测，避免用多个bool拼出含义不明的任务状态。
enum class TaskKind { RecordAudio, EnumerateDevices, ProbeMedia };
struct RecordingJob {
    TaskKind kind = TaskKind::RecordAudio;
    CaptureOptions options;
    std::wstring probeFile;
    MediaInfo mediaInfo; // worker写，GUI在join后读
    // 运行中共享的停止请求：GUI写、worker读。atomic不强杀线程，worker需主动检查并返回。
    std::atomic<bool> cancel{false};
    // 运行中共享进度：worker写、GUI读，单位为每声道样本。
    std::atomic<int64_t> progress{0};
    // 普通结果和设备数组只允许GUI在线程结束且join后读取，不能边写边读。
    CaptureResult result;
    std::vector<AudioDevice> devices;
};

class RecordingController {
public:
    // 窗口创建管理者时从空状态开始，尚未持有任务或需要回收的线程。
    RecordingController() = default;
    // 管理者销毁前调用Shutdown，先停止并等后台退出，避免Job比使用它的线程先消失。
    ~RecordingController();
    // 禁止复制控制器；任务和线程只有一个管理者，不能复制后由两处同时释放。
    RecordingController(const RecordingController&) = delete;
    RecordingController& operator=(const RecordingController&) = delete;
    // Controls/Poll通过这个借用指针查看任务；不能用它转移所有权或提前销毁Job。
    RecordingJob* Current() const { return job_.get(); }
    // StartJob把任务唯一所有权交给这里；旧任务未回收时拒绝再接一份，避免两个worker争用会话。
    void Start(std::unique_ptr<RecordingJob> job);
    // Poll用它零等待检查worker：没结束就返回false，真实退出才join，让GUI随后安全读取普通结果。
    // 观察失败会抛异常并保留任务，不能把超时或观察错误当成线程已经退出。
    bool TryJoin();
    // Poll显示完结果后调用它归还Job；线程还未join时拒绝清空，因为worker可能仍在使用任务。
    void ClearFinished();
    // wmain退出和析构用它兜底：请求取消，真实join，再释放Job；这不是硬超时强杀线程。
    void Shutdown();
private:
    std::unique_ptr<RecordingJob> job_;
    std::thread worker_;
};
}
