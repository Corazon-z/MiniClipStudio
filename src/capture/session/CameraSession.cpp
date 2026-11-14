// S06线程边界集中在这里；窗口只提交请求、读快照和取图，不接触采集输入/编解码器。
#include "CameraSession.h"
#include <Windows.h>
namespace miniclip {
// 析构保持所有成员活到worker退出；不detach，不使用超时后销毁的假回收。
CameraSession::~CameraSession() { Stop(); if (worker_.joinable()) worker_.join(); }
// Launch只能由同一个控制线程调用；Snapshot/TakePicture的共享部分另用mutex保护。
void CameraSession::Launch(CameraTask task, std::function<void()> work) {
    if (worker_.joinable()) throw std::logic_error("Join the previous camera task first");
    stop_.store(false);
    { std::lock_guard lock(mutex_); snapshot_ = {}; snapshot_.task = task; snapshot_.state = CameraState::Working; latest_.reset(); }
    try {
        worker_ = std::thread([this, work = std::move(work)] {
            try {
                work();
                std::lock_guard lock(mutex_); snapshot_.state = CameraState::Completed;
            } catch (const std::exception& error) {
                std::lock_guard lock(mutex_); snapshot_.state = CameraState::Failed; snapshot_.message = error.what();
            } catch (...) {
                std::lock_guard lock(mutex_); snapshot_.state = CameraState::Failed; snapshot_.message = "Unknown camera task failure";
            }
        });
    } catch (...) { std::lock_guard lock(mutex_); snapshot_.state = CameraState::Failed; throw; }
}
// 设备清单先在局部完整建好，再一次交给快照，UI不会看见填到一半的vector。
void CameraSession::QueryDevices() {
    Launch(CameraTask::Devices, [this] {
        auto cameras = ListCameraDevices();
        std::vector<AudioDevice> microphones;
        std::string warning;
        try { microphones = ListAudioDevices(); } catch (const std::exception& error) { warning = error.what(); }
        std::lock_guard lock(mutex_);
        snapshot_.cameras = std::move(cameras); snapshot_.microphones = std::move(microphones);
        snapshot_.message = snapshot_.cameras.empty() ? "No camera / 未检测到摄像头" : "Devices ready / 请选择摄像头及模式";
        if (!warning.empty()) snapshot_.message += "; microphone query: " + warning;
    });
}
// 模式值在任务lambda里复制保存，不能借用GUI临时选择字符串。
void CameraSession::QueryModes(const std::string& camera) {
    Launch(CameraTask::Modes, [this, camera] {
        auto modes = ListCameraModes(camera);
        std::lock_guard lock(mutex_); snapshot_.modes = std::move(modes);
        snapshot_.message = "Modes ready / 模式来自驱动；unsupported不能录制";
    });
}
// 引擎在worker同步执行；状态/画面回调也在worker，全部经短锁交给GUI。
void CameraSession::Start(const CameraCaptureOptions& options) {
    Launch(CameraTask::Capture, [this, options] {
        auto result = RunCameraCapture(options, stop_, [this](PreviewPicture picture) {
            auto shared = std::make_shared<const PreviewPicture>(std::move(picture));
            std::lock_guard lock(mutex_);
            if (latest_) ++snapshot_.replacedPictures;
            latest_ = std::move(shared);
        }, [this](const std::string& message) { std::lock_guard lock(mutex_); snapshot_.message = message; });
        std::lock_guard lock(mutex_); snapshot_.result = result;
        snapshot_.message = result.saved ? "MP4 saved / MP4已完整保存" : "Preview stopped / 预览已停止，未保存文件";
    });
}
// 不需持锁配合条件变量：本会话没有CV等待，倒计时/读取均合作检查这个atomic。
void CameraSession::Stop() { stop_.store(true); }
// Active只由控制线程使用；joinable可能已结束，仍表示需要回收，所以不拿快照终态替代它。
bool CameraSession::Active() const { return worker_.joinable(); }
bool CameraSession::TryJoin() {
    if (!worker_.joinable()) return true;
    const DWORD state = WaitForSingleObject(worker_.native_handle(), 0);
    if (state == WAIT_TIMEOUT) return false;
    if (state != WAIT_OBJECT_0) throw std::runtime_error("Observe camera worker failed");
    worker_.join(); return true;
}
// 即使worker尚未退出，锁保护下的副本也可读；结果是否成功仍由state/result共同判断。
CameraSnapshot CameraSession::Snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
std::shared_ptr<const PreviewPicture> CameraSession::TakePicture() {
    std::lock_guard lock(mutex_); auto picture = std::move(latest_); return picture;
}
}
