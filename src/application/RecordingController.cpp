// 这个文件实现窗口的后台任务生命周期，把S00录音/枚举、S01探测从消息回调移到worker。
// 窗口只提交任务、发停止请求并定时查看结果；本文件负责持有Job、启动线程和确认真实退出。
#include "RecordingController.h"
#include <Windows.h>
#include <stdexcept>
namespace miniclip {
// 管理者销毁时仍按Shutdown顺序停止、join和释放，不能让worker借用一个已不存在的Job。
RecordingController::~RecordingController() { Shutdown(); }
// StartJob调用这里提交任务；先接管Job，再创建worker，按TaskKind调用对应媒体函数。
// 线程创建失败要恢复空状态；线程内的业务异常则写入结果，留给窗口在join后显示。
void RecordingController::Start(std::unique_ptr<RecordingJob> job) {
    if (job_ || worker_.joinable()) throw std::logic_error("Recording task is still active");
    if (!job) throw std::invalid_argument("Missing recording job");
    job_ = std::move(job);
    RecordingJob* shared = job_.get(); // 借用到worker真正退出，不能提前reset。
    try {
        // 新线程借用shared，分别调用ListAudioDevices、ProbeMediaFile或RunAudioCapture。
        // 这些调用完成后写结果；Job仍由控制器持有，异常不能逃出线程入口导致进程终止。
        worker_ = std::thread([shared] {
            try {
                if (shared->kind == TaskKind::EnumerateDevices) {
                    shared->devices = ListAudioDevices();
                    shared->result.success = true;
                } else if (shared->kind == TaskKind::ProbeMedia) {
                    shared->mediaInfo = ProbeMediaFile(shared->probeFile, &shared->cancel);
                    shared->result.success = true;
                } else shared->result = RunAudioCapture(shared->options, shared->cancel, shared->progress);
            } catch (const MediaProbeCancelled& error) {
                shared->result.cancelled = true;
                shared->result.message = error.what();
            } catch (const std::exception& error) { shared->result.message = error.what(); }
        });
    } catch (...) { job_.reset(); throw; }
}
// 窗口的Poll调用这里时不能久等，因此WaitForSingleObject的等待时间设为0。
// 仅在线程句柄表示真实退出后join；join建立同步后，窗口才能读取worker写的普通结果。
bool RecordingController::TryJoin() {
    if (!worker_.joinable()) return true;
    DWORD status = WaitForSingleObject(worker_.native_handle(), 0);
    if (status == WAIT_TIMEOUT) return false;
    if (status != WAIT_OBJECT_0) throw std::runtime_error("Thread observation failed, Windows error " + std::to_string(GetLastError()));
    worker_.join(); // 确认退出后join建立同步，调用者才能读取普通result。
    return true;
}
// Poll消费完结果后调用这里释放Job；只要线程仍需join，就不能删掉它可能使用的任务。
void RecordingController::ClearFinished() {
    if (worker_.joinable()) throw std::logic_error("Cannot discard an unjoined task");
    job_.reset();
}
// wmain和析构用这里处理最终退出：先写取消请求，再等worker返回，最后归还Job。
// join可能真实等待驱动操作结束；不能提前释放任务，也不能detach后冒充已经安全退出。
void RecordingController::Shutdown() {
    if (job_) job_->cancel.store(true);
    if (worker_.joinable()) worker_.join();
    job_.reset();
}
}
