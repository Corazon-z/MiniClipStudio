// 会话/线程管理不依赖窗口控件；此Windows版本用native_handle零等待观察。
#include "RecordingController.h"
#include <Windows.h>
#include <stdexcept>
namespace miniclip {
RecordingController::~RecordingController() { Shutdown(); }
void RecordingController::Start(std::unique_ptr<RecordingJob> job) {
    if (job_ || worker_.joinable()) throw std::logic_error("Recording task is still active");
    if (!job) throw std::invalid_argument("Missing recording job");
    job_ = std::move(job);
    RecordingJob* shared = job_.get(); // 借用到worker真正退出，不能提前reset。
    try {
        worker_ = std::thread([shared] {
            try {
                if (shared->enumerate) {
                    shared->devices = ListAudioDevices();
                    shared->result.success = true;
                } else shared->result = RunAudioCapture(shared->options, shared->cancel, shared->progress);
            } catch (const std::exception& error) { shared->result.message = error.what(); }
        });
    } catch (...) { job_.reset(); throw; }
}
bool RecordingController::TryJoin() {
    if (!worker_.joinable()) return true;
    DWORD status = WaitForSingleObject(worker_.native_handle(), 0);
    if (status == WAIT_TIMEOUT) return false;
    if (status != WAIT_OBJECT_0) throw std::runtime_error("Thread observation failed, Windows error " + std::to_string(GetLastError()));
    worker_.join(); // 确认退出后join建立同步，调用者才能读取普通result。
    return true;
}
void RecordingController::ClearFinished() {
    if (worker_.joinable()) throw std::logic_error("Cannot discard an unjoined task");
    job_.reset();
}
void RecordingController::Shutdown() {
    if (job_) job_->cancel.store(true);
    if (worker_.joinable()) worker_.join();
    job_.reset();
}
}
