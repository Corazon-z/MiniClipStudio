// 错误检查、路径转换及帧/包创建。资源接口见对应头文件。
#include "FfmpegSupport.h"
namespace miniclip {
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    // 第一次调用只查询需要多少字节；返回0表示转换失败。
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode path");
    // 准备足够长度的输出字符串，第二次调用才实际写入转换结果。
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr))
        throw std::runtime_error("Path conversion failed");
    return result;
}

void Check(int code, const char* stage) {
    if (code >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, text, sizeof(text)) < 0) strcpy_s(text, "Unknown FFmpeg error");
    throw std::runtime_error(std::string(stage) + ": " + text);
}

Frame NewFrame() {
    Frame p(av_frame_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}

Packet NewPacket() {
    Packet p(av_packet_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}
}
