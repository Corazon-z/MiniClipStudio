// 这个项目模块负责S04的文件音频解码，AudioPlayer::Produce在生产线程调用DecodeAudioFile。它打开首条音轨、
// 还原声音样本，再用libswresample统一为48kHz双声道float，按L,R,L,R…排列后分块交回。
// 它不操作窗口、播放队列或声卡；这几项分别由入口、AudioPlayer和WasapiOutput负责。
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace miniclip {
inline constexpr int PlaybackRate = 48000;
inline constexpr int PlaybackChannels = 2;
inline constexpr int PcmBlockFrames = 1024;
struct PcmBlock {
    std::array<float, PcmBlockFrames * PlaybackChannels> samples{};
    int frames = 0; // 音频帧=同一时刻左右声道各一个样本，不是视频画面。
};
struct AudioDecodeResult {
    int streamIndex = -1;
    int64_t frames = 0;
};
struct PlaybackCancelled : std::runtime_error {
    // 检查到停止请求时构造此异常，让播放层区分用户叫停与文件/解码失败。
    PlaybackCancelled() : std::runtime_error("Audio playback stopped / 已停止播放") {}
};
// AudioPlayer::Produce调用它读取本地首音轨；打开/探测后反复读包、解码、重采样，每1024帧交给consume。
// 文件末尾先排出解码器和重采样器的剩余样本，再交最后短块，返回选中轨道和累计输出帧数。
// 所有操作和回调都在调用线程同步执行；stop仅借用，PCM块按值交出，不借用FFmpeg帧缓冲。
AudioDecodeResult DecodeAudioFile(const std::wstring& file, const std::atomic<bool>& stop,
    const std::function<void(PcmBlock)>& consume);
}
