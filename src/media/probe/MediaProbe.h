// 这里定义S01媒体信息探测的接口和结果，相当于先读媒体文件的“说明书”，不改文件或完整播放。
// 窗口通过RecordingController在后台调用ProbeMediaFile，CLI的--probe则直接调用它。
// ProbeMediaFile用libavformat打开文件、识别轨道并复制参数，FormatMediaInfo再把结果整理成文字。
// MediaInfo只保存自己的字符串和数字，所以文件关闭后，窗口仍能安全读取这份信息。
#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
namespace miniclip {
struct MediaRational {
    int numerator = 0;   // 分子
    int denominator = 0; // 分母；0表示未知，不可直接相除
};
struct MediaStreamInfo {
    int index = 0;
    std::string type, codecName, codecDescription;
    MediaRational timeBase, averageFrameRate, referenceFrameRate;
    std::optional<double> durationSeconds;
    std::optional<int64_t> bitRate;
    std::optional<int> sampleRate, channels, width, height;
    std::string channelLayout, sampleFormat, pixelFormat, language;
    bool attachedPicture = false; // 封面图片也可能表现为视频流，不假装是动态视频
};
struct MediaInfo {
    std::string path, containerName, containerDescription;
    std::optional<double> durationSeconds;
    std::optional<int64_t> bitRate;
    std::vector<MediaStreamInfo> streams;
};
class MediaProbeCancelled : public std::runtime_error {
public:
    // CheckProbeState用这个异常表示用户停止了探测，让上层与文件损坏或超时分开处理。
    MediaProbeCancelled() : std::runtime_error("Media probe cancelled / 已取消媒体探测") {}
};
// S01的探测入口：先检查路径，再调用avformat_open_input打开文件和avformat_find_stream_info识别轨道，
// 最后复制参数并关闭输入。函数在调用线程内完成，窗口应交给后台执行，失败或取消会抛异常。
// cancel只是借用，必须活到函数返回；15秒超时依靠底层配合中断，不能强制杀掉任意系统调用。
MediaInfo ProbeMediaFile(const std::wstring& path, const std::atomic<bool>* cancel = nullptr);
// 窗口和CLI用它把MediaInfo排成UTF-8报告，给参数补上中文含义和单位；这里不再打开文件或操作窗口。
std::string FormatMediaInfo(const MediaInfo& info);
}
