#pragma once
// 一个WAV输出会话，显式Open/Push/Finish；不拥有窗口或线程。
#include "../ffmpeg/FfmpegSupport.h"
namespace miniclip {
class WavWriter {
public:
    // 构造函数先用成员初始化列表保存目标路径；explicit避免意外隐式转换。
    // 这里只准备名称，不获取FFmpeg资源；后续Open失败时对象已构造，可正常析构清理。
    explicit WavWriter(const std::wstring& destination);
    // 析构只兜底清理，不等于成功保存；真正可失败的业务收尾放在Finish中显式执行。
    ~WavWriter();
    // 删除复制操作：若两个对象持有相同资源地址，会造成重复释放。
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    // 准备输出：WAV上下文 → PCM编码器 → 输出流 → 文件IO → 容器头。每步失败都交析构清理。
    void Open();
    // 接收一个解码后的音频帧；const指针表示不通过此接口修改帧元数据，不接管调用者所有权。
    void Push(const AVFrame* input);
    // 停止后的业务收尾：排空转换器、排空编码器、写WAV尾部、关闭IO、发布最终文件。
    void Finish();
    // 查询每声道样本累计数；const成员函数不修改普通成员，供进度和时长显示使用。
    int64_t Samples() const;
private:
    // 重采样一次；input为空是排空模式，返回实际产生的每声道样本数。
    int Convert(const AVFrame* input);
    // 不断取编码包，直到暂时无包或编码结束；一帧输入并不承诺只产生一个输出包。
    void Receive();
    // 编码端的send/receive配合：若发送要求先取输出，则Receive后重试同一帧，不能提前丢输入。
    void Send(const AVFrame* frame);
    // 成员状态：目标/临时路径是自有字符串；上下文/编码器/转换器由本类释放，stream_仅借用。
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
