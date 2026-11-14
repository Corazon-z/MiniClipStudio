// S06的同步摄像头引擎：真实设备或测试文件读包，连续预览，可同时编码H.264/AAC并安全发布MP4。
// 窗口通过CameraSession放到后台调用；本模块不操作GUI，也不向扬声器监听麦克风，避免回声。
#pragma once
#include "../../platform/windows/capture/CameraCatalog.h"
#include "../../platform/windows/capture/ScreenRegionSelector.h"
#include "../../playback/PreviewTypes.h"
#include "../../export/Transcode.h"
namespace miniclip {
struct CameraCaptureOptions {
    std::string camera, microphone; // 空麦克风表示无声，设备名来自枚举，不是显示序号。
    CameraMode mode;
    std::wstring outputFile; // 空表示只预览；录制必须使用新的.mp4路径。
    int outputFps = 30, countdownSeconds = 0;
    double durationSeconds = 0; // 0表示等停止；真实设备按墙钟限制录制时长。
    std::wstring testInput; // 明确的测试适配口：顺序处理文件，绝不冒称真实硬件。
    // 非空时选用gdigrab区域输入，复用预览/编码/停止发布；区域首版仅录画面，摄像头麦克风行为保留。
    std::optional<ScreenRegion> screenRegion;
};
struct CameraCaptureResult {
    bool saved = false, stopped = false;
    int64_t previewFrames = 0, originUs = 0;
    TranscodeMetrics media;
};
// stop表示“结束采集并保存已收到内容”，不是S03的“取消并撤销成品”；错误仍抛异常。
// picture按值交出自有BGRA；onStatus在本线程通知倒计时/打开/采集/收尾，调用者不可直接改窗口。
CameraCaptureResult RunCameraCapture(const CameraCaptureOptions& options, const std::atomic<bool>& stop,
    const std::function<void(PreviewPicture)>& picture = {}, const std::function<void(const std::string&)>& onStatus = {});
}
