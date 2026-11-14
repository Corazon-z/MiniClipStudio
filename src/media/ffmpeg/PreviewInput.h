// S05的音频和视频解码线程各自拥有一份PreviewInput；这里只负责打开、读包、seek与FFmpeg资源归还。
// 两路独立读包避免某一路队列满时堵住另一轨；首次实现优先清楚的所有权，重复IO成本留待测量。
#pragma once
#include "FfmpegSupport.h"
#include "../../playback/PreviewTypes.h"

namespace miniclip {
class PreviewInput {
public:
    // streamIndex=-1只读取文件信息；非负时同时按选中轨道准备解码器。
    PreviewInput(const std::wstring& file, PreviewReadControl control, int streamIndex = -1);
    PreviewInput(const PreviewInput&) = delete;
    PreviewInput& operator=(const PreviewInput&) = delete;
    // 格式/解码器由内部InputResources拥有，调用方只能在本线程借用。
    AVFormatContext* Format() const { return resources_.format; }
    AVCodecContext* Decoder() const { return resources_.decoder; }
    const AVStream* Stream() const;
    // 使用选中轨道的时间单位回到目标之前，再flush解码器；后续仍需前置解码到目标。
    void Seek(int64_t absoluteUs);
    // 重设单次读取期限并检查取消；EOF原样返回，其他真实错误由CheckResult处理。
    int Read(AVPacket* packet);
    // 每个FFmpeg调用后先识别请求替换，再检查真实错误，避免旧请求结束新会话。
    void CheckResult(int code, const char* stage) const;
private:
    // FFmpeg读/定位过程中只查询标志与期限，回调不得抛异常。
    static int Interrupt(void* opaque);
    // 仅在一次输入IO返回后核查期限；解码或队列等待不能误用上一次读取的旧期限。
    void CheckIoResult(int code, const char* stage) const;
    PreviewReadControl control_;
    std::chrono::steady_clock::time_point deadline_;
    int streamIndex_ = -1;
    InputResources resources_;
};
// VideoPlayer打开文件时调用一次，选择首条非封面视频和可选首音轨，复制共同起点和时长。
PreviewSource ReadPreviewSource(const std::wstring& file, PreviewReadControl control);
}
