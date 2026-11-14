// S03的ExportMp4为选中的视频和音轨各建一个TranscodeTrack，每个对象负责一条轨道从源包到成品包的加工。
// PushPacket先用libavcodec解码，解出的帧交libavfilter裁剪、转换，再用H.264或AAC编码器重新编码，
// 最后把包交Mp4Output封装。Finish按顺序取完各级暂存的尾部数据，不让文件结束时丢帧或丢样本。
// 本对象拥有编解码器、过滤器图和复用帧包；源轨道、输出会话、选项、指标、取消标志都由调用方保管。
#pragma once
#include "../../export/Transcode.h"
#include "../mux/Mp4Output.h"
#include <map>
extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

namespace miniclip {
// 只在本次转码的调用线程使用，借用的对象必须活得更久；这里不会自行创建另一个后台线程。
class TranscodeTrack {
public:
    // ExportMp4创建轨道时先准备源解码器、目标编码器和输出流；过滤器等第一帧到来后再按真实格式建立。
    TranscodeTrack(const AVStream* source, Mp4Output& output, const TranscodeOptions& options,
        int64_t clipStartUs, TranscodeMetrics& metrics, const std::atomic<bool>* cancel,
        std::function<void(const AVFrame*)> decodedVideo = {});
    // 析构归还本轨的编解码和过滤资源，不代替Finish处理尾部，也不替Mp4Output写文件尾。
    ~TranscodeTrack();
    // 禁止复制资源所有权，防止两个轨道对象重复释放同一组FFmpeg资源。
    TranscodeTrack(const TranscodeTrack&) = delete;
    // 赋值也不能复制这组资源的释放责任。
    TranscodeTrack& operator=(const TranscodeTrack&) = delete;
    // ExportMp4读到本轨的包后调用它，送入解码器，再让已经产出的帧沿过滤、编码、封装继续向后走。
    void PushPacket(const AVPacket* packet);
    // ExportMp4读完输入后调用它，依次通知解码器、过滤器、编码器结束，并把各级缓存中的尾部取完。
    void Finish();
    // ExportMp4用这个源流编号判断读到的包是否属于本轨，不把视频包送进音频解码器。
    int SourceIndex() const { return source_->index; }
    // 返回构造时确定的轨道类型，true是视频、false是音频，不重新探测文件。
    bool IsVideo() const { return video_; }
private:
    // PushPacket和Finish通过它循环接收解码帧，再交ProcessDecoded检查并送过滤器。
    int ReceiveDecoded();
    // 检查解码帧的时间戳和实际格式，首帧调用OpenFilters，随后把帧送入过滤器并取结果。
    void ProcessDecoded(AVFrame* frame);
    // ProcessDecoded在首帧到来时调用它，按真实输入格式建立视频buffer或音频abuffer过滤器图。
    void OpenFilters(const AVFrame* first);
    // 取出裁剪/转换后的帧，换算PTS这一播放时刻及有效时长，再交SendEncoded重新编码。
    int ReceiveFiltered();
    // 向编码器送帧；暂时不能接收时先通过ReceiveEncoded取走结果，再重送当前帧。
    void SendEncoded(AVFrame* frame);
    // 取出成品编码包，换算时间单位、检查DTS解码先后顺序，再交Mp4Output写进MP4。
    int ReceiveEncoded();
    // 每轮处理检查用户的停止标志，发现请求就抛取消异常，沿外层会话退出并清理。
    void CheckCancel() const;
    // 构造失败和析构都通过它释放已取得的资源；释放函数可接受空指针，清理过程不向外抛异常。
    void Cleanup() noexcept;
    const AVStream* source_;
    Mp4Output& output_;
    const TranscodeOptions& options_;
    TranscodeMetrics& metrics_;
    const std::atomic<bool>* cancel_;
    // S06同步借用已解码视频做预览；回调不得保存AVFrame裸地址，S03默认不使用。
    std::function<void(const AVFrame*)> decodedVideo_;
    bool video_, filterEnded_ = false;
    int64_t clipStartUs_, lastInputPts_ = AV_NOPTS_VALUE, lastDts_ = AV_NOPTS_VALUE;
    AVCodecContext* decoder_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    AVStream* destination_ = nullptr; // 随Mp4Output释放
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* filterSource_ = nullptr;
    AVFilterContext* filterSink_ = nullptr;
    Frame decoded_, filtered_;
    Packet encoded_;
    // 编码可能延迟输出；按PTS保留有效时长，直到相应包产生，避免尾帧越过裁剪终点。
    std::map<int64_t, int64_t> durations_;
    int firstWidth_ = 0, firstHeight_ = 0, firstFormat_ = -1, firstRate_ = 0;
    std::string firstLayout_;
};
}
