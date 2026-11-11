#pragma once
// FFmpeg内部资源与错误工具；worker独占资源，禁止复制输入上下文。
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
// C 链接约定，用于链接 FFmpeg C API，不是支持函数重载。
extern "C" {
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}

namespace miniclip {

// 把Windows宽字符串转换为FFmpeg使用的UTF-8；不靠强转指针改变编码。
std::string Utf8(const std::wstring& value);
// 统一检查FFmpeg返回值：本处约定负值为错误，非负值成功。
// EAGAIN/EOF等有专门意义的状态必须由调用处先处理，不能都送来当致命错误。
void Check(int code, const char* stage);
// 智能指针的自定义删除器：FFmpeg分配的对象必须用对应free接口，而不是delete。
// operator()让这个小对象像函数一样可调用；释放函数需要指针变量的地址。
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
// 继承标准异常以单独表示“零样本”；不能把真正的磁盘/编码错误也归类为取消。
struct NoSamples : std::runtime_error {
    NoSamples() : std::runtime_error("No audio samples recorded; no WAV file was saved") {}
};
// using为长类型起别名；unique_ptr表达唯一所有权，离开作用域自动调用上面的删除器。
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
// 创建帧对象并检查内存分配；return交回智能指针所有权。帧缓冲稍后单独申请。
Frame NewFrame();
// 包对象可反复装入数据；使用后unref释放内容，最终free由智能指针负责。
Packet NewPacket();
// 局部RAII资源集合：输入会话、解码器和选项字典均由当前工作线程拥有。
// 析构会处理正常返回与异常退出；当前只在局部使用，不应复制这些资源裸指针。
struct InputResources {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVDictionary* options = nullptr;
    // 创建空状态并禁止复制，避免未来传参时误复制资源裸指针。
    InputResources() = default;
    InputResources(const InputResources&) = delete;
    InputResources& operator=(const InputResources&) = delete;
    // 析构处理正常返回与异常展开；不等于业务文件保存成功。
    ~InputResources() {
        av_dict_free(&options);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    }
};

}
