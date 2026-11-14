// 这里实现S03的MP4文件输出。ExportMp4安排写头、写包、收尾和发布，TranscodeTrack只交已经编码的包，
// libavformat负责容器组织，而本模块的Write/Seek回调负责将字节写到独占创建的Windows临时文件。
// 轨道只借用format_/pb；清理时先断开借用，再分别归还容器、AVIO缓冲和文件，未完成的文件不会发布。
#define NOMINMAX
#include "Mp4Output.h"
#include <cerrno>
#include <filesystem>

namespace miniclip {
// ExportMp4创建输出会话时，先用CREATE_NEW独占打开临时文件，再创建MP4上下文和128KiB AVIO缓冲，
// 最后把Write/Seek接为FFmpeg回调。构造中途失败不会自动调用本对象析构，因此catch必须主动Cleanup。
Mp4Output::Mp4Output(const std::wstring& destination) : destination_(destination) {
    static std::atomic<uint64_t> sequence{0};
    temporary_ = destination + L".part-" + std::to_wstring(GetCurrentProcessId()) + L"-"
        + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
    try {
        file_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Create export temporary file failed");
        owned_ = true;
        Check(avformat_alloc_output_context2(&format_, nullptr, "mp4", nullptr), "Create MP4 output");
        if (!format_) throw std::bad_alloc();
        auto* buffer = static_cast<unsigned char*>(av_malloc(128 * 1024));
        if (!buffer) throw std::bad_alloc();
        io_ = avio_alloc_context(buffer, 128 * 1024, 1, this, nullptr, Write, Seek);
        if (!io_) { av_free(buffer); throw std::bad_alloc(); }
        format_->pb = io_; // 借用；Cleanup负责释放，format不拥有自定义IO
        format_->flags |= AVFMT_FLAG_CUSTOM_IO;
    } catch (...) { Cleanup(); throw; }
}
// ExportMp4结束或异常离开时统一Cleanup；析构只归还资源，不把未完成的临时文件自动改成成品。
Mp4Output::~Mp4Output() { Cleanup(); }
// 构造失败和析构共用这条清理路径：先断开format对自定义pb的借用，再分别释放容器和AVIO，
// 然后关闭句柄，只删除本会话拥有且尚未发布的临时文件。清理失败记诊断，不向析构外抛异常。
void Mp4Output::Cleanup() noexcept {
    if (format_) { format_->pb = nullptr; avformat_free_context(format_); format_ = nullptr; }
    if (io_) { av_freep(&io_->buffer); avio_context_free(&io_); }
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    if (owned_ && !published_ && !DeleteFileW(temporary_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
        std::fprintf(stderr, "Export cleanup failed: %lu\n", GetLastError());
    owned_ = false;
}
// FFmpeg封装时经AVIO调用Write，用WriteFile写到本会话句柄并核对长度；失败或短写返回AVERROR(EIO)。
// 这里确认的是本次写入被接受，不等于系统缓冲已经刷新；Finish才显式刷新。C回调不能抛C++异常。
int Mp4Output::Write(void* opaque, const uint8_t* data, int size) {
    auto* self = static_cast<Mp4Output*>(opaque);
    DWORD written = 0;
    if (size < 0 || !WriteFile(self->file_, data, static_cast<DWORD>(size), &written, nullptr)
        || written != static_cast<DWORD>(size)) return AVERROR(EIO);
    return size;
}
// FFmpeg写MP4时可能查询总大小，或回到前面补写容器说明；分别用GetFileSizeEx和SetFilePointerEx完成。
// AVSEEK_SIZE只查询而不移动位置，SEEK_SET/CUR/END对应不同定位基准，不认识的参数返回错误码。
int64_t Mp4Output::Seek(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<Mp4Output*>(opaque);
    LARGE_INTEGER value{}, position{};
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return GetFileSizeEx(self->file_, &value) ? value.QuadPart : AVERROR(EIO);
    DWORD method;
    if (whence == SEEK_SET) method = FILE_BEGIN;
    else if (whence == SEEK_CUR) method = FILE_CURRENT;
    else if (whence == SEEK_END) method = FILE_END;
    else return AVERROR(EINVAL);
    value.QuadPart = offset;
    return SetFilePointerEx(self->file_, value, &position, method) ? position.QuadPart : AVERROR(EIO);
}
// ExportMp4创建好各输出轨道后调用avformat_write_header，写下容器头和轨道说明。
// 成功才标记header_，重复写头或写头失败不能继续当成正常会话写包。
void Mp4Output::WriteHeader() {
    if (header_) throw std::logic_error("MP4 header already written");
    Check(avformat_write_header(format_, nullptr), "Write MP4 header");
    header_ = true;
}
// TranscodeTrack接收到编码包并换算好时间后调用这里，av_interleaved_write_frame按轨道时间组织到MP4中。
// 包对象仍归轨道管理，但包内容引用会被FFmpeg消费；未写头或已经Finish时拒绝再写。
void Mp4Output::WritePacket(AVPacket* packet) {
    if (!header_ || finished_) throw std::logic_error("MP4 packet outside output session");
    Check(av_interleaved_write_frame(format_, packet), "Mux MP4 packet");
}
// ExportMp4排空所有轨道后，先av_write_trailer完成MP4说明，再avio_flush清空FFmpeg IO缓冲并检查错误，
// 接着FlushFileBuffers显式刷新系统文件缓冲，取得大小并关句柄。这里只完成临时文件，之后仍须检查取消再发布。
void Mp4Output::Finish() {
    if (!header_ || finished_) throw std::logic_error("MP4 finish outside output session");
    Check(av_write_trailer(format_), "Write MP4 trailer");
    avio_flush(io_);
    Check(io_->error, "Flush MP4 IO");
    if (!FlushFileBuffers(file_)) throw std::runtime_error("Flush export file failed");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size)) throw std::runtime_error("Read export size failed");
    bytes_ = static_cast<uint64_t>(size.QuadPart);
    if (!CloseHandle(file_)) throw std::runtime_error("Close export file failed");
    file_ = INVALID_HANDLE_VALUE;
    finished_ = true;
}
// ExportMp4确认收尾完成且未取消后调用Publish，用不带覆盖标志的MoveFileExW改成最终文件名。
// 同名目标已存在就失败；只有改名成功才标记published_，让析构知道这个文件已经交给用户。
void Mp4Output::Publish() {
    if (!finished_ || published_) throw std::logic_error("MP4 publish outside output session");
    if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Publish MP4 failed / 目标可能已存在");
    published_ = true;
}
}
