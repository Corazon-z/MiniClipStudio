// 这是项目自己写的FFmpeg辅助模块，不是FFmpeg库本身。录音以及S01—S04共用Utf8、Check、NewFrame、NewPacket，
// 集中完成路径转换、错误检查和资源管理，避免各处重复处理“路径怎么传、错误怎么解释、用完怎么释放”。
// 它不决定要录音、探测还是播放；具体任务仍在业务模块中完成，媒体上下文由执行任务的线程独占。
#pragma once
#include <atomic>
#include <string>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
// 下面才是真正的第三方FFmpeg头文件。extern "C"使C++按FFmpeg的C接口链接，具体职责分开说明。
extern "C" {
// libavformat负责读写容器：S01读取文件/轨道信息，S02—S04拆出编码包，WAV和S03 MP4输出也靠它封装。
#include <libavformat/avformat.h>
// libavdevice把设备接入FFmpeg；已有录音用它枚举/打开DirectShow麦克风，S04扬声器输出则使用WASAPI。
#include <libavdevice/avdevice.h>
// libavcodec负责包与帧之间的编解码；S02还原视频帧并编码PNG，S03编码H.264/AAC，S04把音轨包还原成声音样本。
// AVPacket装一包编码数据，音频包不保证是PCM；AVFrame描述可处理的声音样本或图像及其规格。
#include <libavcodec/avcodec.h>
// libavutil提供公共数据和辅助运算：声道布局说明左右如何排列，错误工具把错误码翻成文字，数学工具换算时间单位。
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
// libswresample负责音频重采样/声道/样本格式转换；录音统一成48k/S16/stereo，S04统一成48k/float/stereo。
#include <libswresample/swresample.h>
}

namespace miniclip {

// 探测、导出和播放在把Windows路径交给FFmpeg前调用Utf8，把UTF-16宽字符串转换成UTF-8字节串。
// 编码不合法就抛异常，不能靠指针强转让同一串字节自动变成另一种文字编码。
std::string Utf8(const std::wstring& value);
// 各媒体步骤用Check检查FFmpeg返回值；失败时抛出“操作阶段+错误原因”，供GUI/CLI显示。
// EAGAIN和EOF有时是正常的推进/结束信号，应由调用处先处理，不能一律当成错误。
void Check(int code, const char* stage);
// FFmpeg资源必须匹配其free接口；下面两个删除器由unique_ptr自动调用。
struct FrameDelete {
    // Frame智能指针销毁时调用av_frame_free，归还帧对象和它持有的数据引用，不能换成delete。
    void operator()(AVFrame* p) const { av_frame_free(&p); }
};
struct PacketDelete {
    // Packet智能指针销毁时调用av_packet_free，归还包对象和仍由它持有的编码缓冲。
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};
// 空录音用这个异常单独报告，调用者据此判断没有声音数据，不把真正的磁盘/编码故障当成取消。
struct NoSamples : std::runtime_error {
    // WavWriter::Finish发现没有声音样本时构造此异常，上层可据此区分“空录音”与真正的写盘失败。
    NoSamples() : std::runtime_error("No audio samples recorded; no WAV file was saved") {}
};
// Frame和Packet把释放责任交给智能指针；每轮unref清内容，离开作用域时再由删除器归还整个对象。
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
// 编解码步骤调用NewFrame取得一个由智能指针管理的AVFrame；只创建帧的描述对象，样本/像素缓冲另行准备。
// 分配失败抛bad_alloc，调用者不必再手写av_frame_free收尾。
Frame NewFrame();
// 读包和编码步骤调用NewPacket取得一个可自动释放的AVPacket，随后可反复装入编码数据。
// 每轮av_packet_unref只清空内容，最后由智能指针归还整个对象；分配失败抛bad_alloc。
Packet NewPacket();
// 这个对象把当前线程的输入会话、解码器和选项放在一起保管，离开作用域时自动释放（RAII）。
// 正常返回或抛异常都会收回资源，因此不能把内部指针复制给另一份所有者。
struct InputResources {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVDictionary* options = nullptr;
    // 录音/文件处理先建空资源集合，再随打开输入、配置解码器逐步填入成员。
    InputResources() = default;
    // 不允许复制会话，否则两个对象会重复释放同一组FFmpeg资源。
    InputResources(const InputResources&) = delete;
    // 赋值也不能复制这组上下文的所有权。
    InputResources& operator=(const InputResources&) = delete;
    // 退出当前处理作用域时依次释放选项字典、解码器和输入会话，异常离开也会执行。
    // 这里保证输入资源归还，不负责证明输出文件已经成功保存。
    ~InputResources() {
        av_dict_free(&options);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    }
};

}
