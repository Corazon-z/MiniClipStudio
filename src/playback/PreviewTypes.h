// S05用这些普通数据把文件解码、播放调度和窗口绘制连接起来。
// 时间都先减去同一个文件起点；generation标记结果属于哪次播放/跳转，旧结果不能进入新画面。
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace miniclip {
inline constexpr int PreviewRate = 48000;
inline constexpr int PreviewChannels = 2;
inline constexpr int PreviewBlockFrames = 1024;
struct PreviewSource {
    std::wstring path;
    int videoStream = -1, audioStream = -1;
    int64_t originUs = 0;
    std::optional<int64_t> durationUs;
};
struct PreviewReadControl {
    const std::atomic<bool>& stop;
    const std::atomic<uint64_t>& requestedGeneration;
    uint64_t generation;
    // 读文件和等待入队时都检查同一个请求，停止或新跳转到来后丢掉本次在途工作。
    bool Interrupted() const { return stop.load() || requestedGeneration.load() != generation; }
    // 同步解码函数用专用异常返回调度层，不把用户跳转误报成坏文件。
    void Check() const;
};
struct PreviewInterrupted : std::runtime_error {
    // 请求被替换或停止时，只取消当前解码代次，下一代仍可重新打开输入。
    PreviewInterrupted() : std::runtime_error("Preview request superseded or stopped") {}
};
struct PreviewPicture {
    uint64_t generation = 0;
    int64_t ptsUs = 0, durationUs = 0;
    bool finalFrame = false; // 末图可保持到声音结束；seek到纯音频尾段也重新解出本代末图。
    int width = 0, height = 0;
    std::vector<uint8_t> bgra; // 自上向下，每行width*4字节，像素由本对象拥有。
};
struct PreviewAudioBlock {
    uint64_t generation = 0;
    int64_t startSample = 0; // 文件共同原点后的48kHz音频帧编号，不是字节位置。
    int frames = 0;
    std::array<float, PreviewBlockFrames * PreviewChannels> samples{};
};
struct PreviewDecodeResult {
    int64_t frames = 0;
    int64_t endUs = 0;
};
}
