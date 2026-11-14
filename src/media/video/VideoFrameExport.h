// 这里定义S02“视频解码与首帧PNG导出”的接口：取出视频第一张画面，继续解完整段并统计画面数量。
// CLI的--video-frame调用ExportFirstVideoFrame，libavformat读视频包，libavcodec还原画面，
// libswscale把首帧转为RGB，PNG编码器再生成真正的图片文件。整段解码成功后才发布图片。
// 这些操作在调用线程内完成；不修改原片，也不承担播放器的显示、定时或音画同步。
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace miniclip {
struct VideoFrameExportResult {
    int streamIndex = -1;
    int width = 0, height = 0;
    int64_t decodedFrames = 0;
    std::optional<double> firstFrameSeconds;
    std::wstring outputFile;
};
class VideoExportCancelled : public std::runtime_error {
public:
    // CheckCancelled用它向调用方报告用户停止，让取消与坏文件、编码失败保持不同含义。
    VideoExportCancelled() : std::runtime_error("Video export cancelled / 已取消视频帧导出") {}
};
// S02的入口会跳过封面，解码第一条视频轨；首帧先编码到临时PNG，其余画面继续校验和计数。
// 输入结束后还要取出解码器暂存的尾部画面，全部成功才交付图片，不覆盖已有文件。
// cancel只是借用，须活到函数返回；未来接入窗口时应放后台线程，不能在按钮回调里长时间解码。
VideoFrameExportResult ExportFirstVideoFrame(const std::wstring& inputFile,
    const std::wstring& outputPng, const std::atomic<bool>* cancel = nullptr);
}
