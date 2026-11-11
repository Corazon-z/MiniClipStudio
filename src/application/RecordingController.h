#pragma once
// GUI线程拥有控制器；worker借用Job。Current仅供GUI线程调用。
// atomic字段可并发访问，其余结果只能TryJoin成功后读取。
#include "../capture/audio/AudioRecorder.h"
#include <memory>
#include <thread>
namespace miniclip {
struct RecordingJob {
    // true执行设备枚举，false执行录音；启动前设定，worker运行期间不修改。
    bool enumerate = false;
    CaptureOptions options;
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
    RecordingController() = default;
    ~RecordingController();
    RecordingController(const RecordingController&) = delete;
    RecordingController& operator=(const RecordingController&) = delete;
    RecordingJob* Current() const { return job_.get(); }
    void Start(std::unique_ptr<RecordingJob> job);
    bool TryJoin(); // 零等待；成功后普通结果才可读。异常不丢弃会话。
    void ClearFinished();
    void Shutdown(); // 真实等待，供程序退出兜底；不冒充硬超时取消。
private:
    std::unique_ptr<RecordingJob> job_;
    std::thread worker_;
};
}
