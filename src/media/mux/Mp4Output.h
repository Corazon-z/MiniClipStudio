// Mp4Output是项目为S03编写的输出模块。ExportMp4创建它，TranscodeTrack把编码包交给它，
// 再由libavformat组织成MP4。实际文件由本模块独占创建，通过自定义AVIO把FFmpeg写入接到Windows文件句柄。
// ExportMp4最后依次Finish、检查取消并Publish，先完成临时文件再发布；S04播放不使用这个输出文件模块。
#pragma once
#include "../ffmpeg/FfmpegSupport.h"

namespace miniclip {
// 独占临时文件+自定义AVIO，使MP4封装器可写包/回填文件说明。
// Context仅借用；编码轨道不能释放它或pb。必须先销毁轨道，再销毁本对象。
class Mp4Output {
public:
    // ExportMp4创建输出时，用CREATE_NEW独占建临时文件，再把Write/Seek回调接入FFmpeg的AVIO。
    // 文件或IO只完成部分初始化也会清理，不能留下无人管理的句柄或缓冲。
    explicit Mp4Output(const std::wstring& destination);
    // ExportMp4离开作用域时执行Cleanup，归还资源并清除未发布临时文件，不自动补做发布。
    ~Mp4Output();
    // 文件、IO和容器只能归一个会话所有，不能复制后重复清理。
    Mp4Output(const Mp4Output&) = delete;
    // 赋值同样不能复制文件句柄与缓冲所有权。
    Mp4Output& operator=(const Mp4Output&) = delete;
    // TranscodeTrack借用这个上下文创建输出流；轨道不能释放它或pb，也不能比Mp4Output活得更久。
    AVFormatContext* Context() const { return format_; }
    // ExportMp4在各轨道参数就绪后写MP4头，之后TranscodeTrack才能把编码包交进来。
    void WriteHeader();
    // TranscodeTrack已编码并换算好时间的包由这里交错写入MP4，只能在写头之后、Finish之前调用。
    // 包对象仍归调用者；av_interleaved_write_frame会消费包内容引用，不能把对象所有权也一起转交。
    void WritePacket(AVPacket* packet);
    // ExportMp4排完所有轨道后调用Finish，写尾、刷新FFmpeg/系统缓冲、记录文件大小并关句柄。
    // 此时只完成临时文件，调用方还可检查取消，再决定是否Publish。
    void Finish();
    // ExportMp4确认任务未取消后，把已Finish的临时文件不覆盖改名；目标已存在就报错。
    void Publish();
    // ExportMp4用Finish记录的最终字节数填写导出指标，这不是正在增长的实时文件大小。
    uint64_t Bytes() const { return bytes_; }
private:
    // FFmpeg的AVIO通过Write写本次字节；回调只能返回长度/负错误码，不能让C++异常穿过C接口。
    static int Write(void* opaque, const uint8_t* data, int size);
    // FFmpeg封装器通过Seek查询大小或重新定位，以便回填MP4说明；失败同样只返回错误码。
    static int64_t Seek(void* opaque, int64_t offset, int whence);
    // 构造失败或析构时统一释放容器、AVIO和句柄，只删除本会话拥有且未发布的临时文件。
    void Cleanup() noexcept;
    std::wstring destination_, temporary_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    AVFormatContext* format_ = nullptr;
    AVIOContext* io_ = nullptr;
    bool owned_ = false, header_ = false, finished_ = false, published_ = false;
    uint64_t bytes_ = 0;
};
}
