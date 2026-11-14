// 这里定义S03单素材转码的任务单、进度和结果：把已有视频按尺寸、帧率、片段要求重新生成H.264/AAC的MP4。
// CLI的--export由RunTranscodeCommand解析参数，再调用ExportMp4；它负责读文件、选择轨道并创建TranscodeTrack，
// 每条轨道通过FFmpeg解码、过滤和编码，最终交Mp4Output封装保存。这里的“转码”不是简单复制文件字节。
// ExportMp4在调用线程内运行；将来接入窗口应交给后台，进度回调也不能直接操作GUI。
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace miniclip {
struct TranscodeOptions {
    std::wstring inputFile, outputFile;
    int width = 0, height = 0; // 同时为0：沿用输入尺寸；同时非0：指定偶数尺寸
    int framesPerSecond = 30; // 本阶段输出恒定整数帧率
    bool includeAudio = true; // 有音轨时取第一条；false只导出画面
    int sampleRate = 48000, channels = 2;
    double startSeconds = 0; // 相对输入共同起点，非系统时间
    double durationSeconds = 0; // 0：一直到输入结束
    int processingThreads = 1; // 解码/过滤/编码的共享配置，不是整个进程的线程总数
};
struct TranscodeProgress {
    int64_t videoFrames = 0, audioSamples = 0;
    double mediaSeconds = 0;
};
struct TranscodeMetrics {
    double totalSeconds = 0, cpuSeconds = 0;
    double decodeSeconds = 0, filterSeconds = 0, encodeSeconds = 0, muxSeconds = 0;
    uint64_t peakWorkingSetBytes = 0, outputBytes = 0;
    int64_t videoFrames = 0, audioSamples = 0, decodedVideoFrames = 0;
    int width = 0, height = 0, audioTracks = 0;
};
class TranscodeCancelled : public std::runtime_error {
public:
    // ExportMp4及其轨道在用户停止时抛出这个异常，让调用方区分主动取消与编码、写盘故障。
    TranscodeCancelled() : std::runtime_error("Export cancelled / 已取消导出，未发布输出") {}
};
// S03的公开入口由--export调用，处理首条非封面视频轨和可选首音轨；写完MP4文件尾、关闭并发布后才返回指标。
// cancel和onProgress只在调用期间借用，回调也在本线程执行；失败或停止会抛异常，不交付半成品或覆盖旧文件。
TranscodeMetrics ExportMp4(const TranscodeOptions& options, const std::atomic<bool>* cancel = nullptr,
    const std::function<void(const TranscodeProgress&)>& onProgress = {});
}
