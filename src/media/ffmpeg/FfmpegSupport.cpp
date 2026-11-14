// 这里实现录音和S01—S04共用的Utf8、Check、NewFrame和NewPacket，业务代码仍决定具体处理步骤。
// 它们分别借助Windows转码API、libavutil错误工具和FFmpeg分配接口，处理路径、报错与帧/包资源寿命。
#include "FfmpegSupport.h"
namespace miniclip {
// ProbeMediaFile、视频导出和音频读写等调用方需要UTF-8路径；先用WideCharToMultiByte查询长度，
// 再分配并转换为独立字符串。任一步失败就抛异常，不让FFmpeg拿残缺路径继续打开文件。
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    // 第一次调用只查目标字节数；空字符串已在上面处理，因此这里返回0就是转换失败。
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode path");
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr))
        throw std::runtime_error("Path conversion failed");
    return result;
}

// 媒体代码在FFmpeg调用后交回返回值；非负值放行，负值通过av_strerror解释并加上操作阶段。
// 异常最终由任务或入口捕获，用户才能知道是打开、解码还是写文件失败。
void Check(int code, const char* stage) {
    if (code >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, text, sizeof(text)) < 0) strcpy_s(text, "Unknown FFmpeg error");
    throw std::runtime_error(std::string(stage) + ": " + text);
}

// 解码/转换/编码需要帧对象时调用av_frame_alloc，再交给Frame智能指针管理。
// 返回的只是空帧描述，数据缓冲由接下来的解码器或av_frame_get_buffer准备；分配失败立即抛异常。
Frame NewFrame() {
    Frame p(av_frame_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}

// av_read_frame或编码接收端需要包对象时，先调用av_packet_alloc并交给Packet智能指针。
// 对象可以多轮复用，离开作用域再统一释放，分配失败不会返回一个空指针让业务继续使用。
Packet NewPacket() {
    Packet p(av_packet_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}
}
