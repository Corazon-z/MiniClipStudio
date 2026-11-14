// WavWriter是项目自己的WAV保存模块，已有录音和--convert的RunAudioCapture通过Open、Push、Finish调用它。
// 它先用libswresample把解码样本转成48kHz/S16/双声道，再经libavcodec和libavformat写成WAV临时文件，
// 完成后才发布最终文件。S04播放直接把PCM送设备，不经过本模块；本对象也不创建窗口或线程。
#pragma once
#include "../ffmpeg/FfmpegSupport.h"
namespace miniclip {
class WavWriter {
public:
    // RunAudioCapture或合成音频实验创建写入器时，只保存目标路径并生成临时名。
    // 真正打开文件留给Open，这样Open即使失败，已构造的对象仍能正常析构清理。
    explicit WavWriter(const std::wstring& destination);
    // 退出录音/转换作用域时归还转换器、编码器、容器及IO，并删除未发布临时文件。
    // 析构只兜底清理，可能失败的保存收尾必须显式调用Finish并检查结果。
    ~WavWriter();
    // 不允许复制输出会话，否则两个对象会重复释放同一文件及FFmpeg上下文。
    WavWriter(const WavWriter&) = delete;
    // 赋值同样不能复制这组输出资源的所有权。
    WavWriter& operator=(const WavWriter&) = delete;
    // 录音准备阶段调用Open建立WAV容器、PCM编码器和输出流，再打开临时文件并写头。
    // 一个对象只能尝试Open一次，成功后才能向Push交声音帧。
    void Open();
    // AudioRecorder取出解码帧后调用Push；首帧决定真实输入规格，后续Convert统一规格并编码写入。
    // 输入帧只借用、不接管；后续格式若变化就报错，不能用旧规格继续解释新声音。
    void Push(const AVFrame* input);
    // AudioRecorder停止读包并排完解码器后调用Finish，继续排空swr和编码器、写尾、关IO，再不覆盖发布。
    // 空录音或任何写入/发布失败都抛异常，因此不能把“已停止采集”直接显示成“已保存成功”。
    void Finish();
    // AudioRecorder用每声道累计有效样本数更新进度；除以48000就是已处理的音频秒数。
    int64_t Samples() const;
private:
    // Push或Finish调用Convert执行一次swr_convert，把实际输出送给编码器并返回有效样本数。
    // input为空表示取出重采样器剩余样本，预留缓冲容量不能当作真实音频长度。
    int Convert(const AVFrame* input);
    // Send调用Receive取完编码器当前可用包，换算时间单位后写入WAV；EAGAIN/EOF结束本轮。
    // 一帧输入不保证只产生一个包，因此这里必须循环接收。
    void Receive();
    // Convert用Send提交有效音频帧，Finish则提交空帧通知结束；随后由Receive取走编码包。
    // 发送遇EAGAIN要先接收再重送原帧，不能丢掉还未被编码器接收的声音。
    void Send(const AVFrame* frame);
    // 路径、输出上下文、编码器和转换器都由本对象持有；stream_只借用，随output_释放。
    std::wstring destination_, temporary_;
    bool openAttempted_ = false, opened_ = false, finishing_ = false;
    AVFormatContext* output_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    AVStream* stream_ = nullptr; // 借用，随 output_ 释放
    SwrContext* resampler_ = nullptr;
    AVChannelLayout inputLayout_{};
    int inputRate_ = 0, inputFormat_ = -1;
    int64_t samples_ = 0;
    bool published_ = false;
};
}
