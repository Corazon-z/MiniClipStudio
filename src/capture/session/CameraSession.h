// S06窗口用这个会话拥有后台线程、停止请求、状态和最新画面；FFmpeg资源只在工作线程内存在。
// GUI用TryJoin确认真实退出后再启动下一次任务，不能仅凭业务状态提前删除会话。
#pragma once
#include "../camera/CameraCapture.h"
#include "../DeviceCatalog.h"
#include <mutex>
#include <thread>
namespace miniclip {
enum class CameraTask { Devices, Modes, Capture };
enum class CameraState { Idle, Working, Completed, Failed };
struct CameraSnapshot {
    CameraTask task = CameraTask::Devices;
    CameraState state = CameraState::Idle;
    std::string message;
    CameraCaptureResult result;
    uint64_t replacedPictures = 0; // GUI没取走时新图替换旧图；不表示录制丢帧。
    std::vector<CameraDevice> cameras;
    std::vector<AudioDevice> microphones;
    std::vector<CameraMode> modes;
};
class CameraSession {
public:
    CameraSession() = default;
    // 退出兜底必须真实join；正常窗口先异步Stop并轮询，避免在析构阶段长时间等驱动。
    ~CameraSession();
    CameraSession(const CameraSession&) = delete;
    CameraSession& operator=(const CameraSession&) = delete;
    // 只排后台任务：设备/模式查询和采集都不能占住GUI消息处理。
    void QueryDevices();
    void QueryModes(const std::string& camera);
    void Start(const CameraCaptureOptions& options);
    // 原子请求停止；采集会保存已有内容，查询不可假装已经退出。
    void Stop();
    // 控制线程零等待观察系统线程，只有真正结束才join并返回true。
    bool TryJoin();
    bool Active() const;
    // 短锁复制一致状态；画面转移共享所有权，不借用工作线程临时缓冲。
    CameraSnapshot Snapshot() const;
    std::shared_ptr<const PreviewPicture> TakePicture();
private:
    // 为三个公开入口统一建立线程边界，捕获错误并保留普通资源的所有权。
    void Launch(CameraTask task, std::function<void()> work);
    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    CameraSnapshot snapshot_;
    std::shared_ptr<const PreviewPicture> latest_;
};
}
