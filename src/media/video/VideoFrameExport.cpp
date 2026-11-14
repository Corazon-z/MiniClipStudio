// S02在这里完成“视频里取首帧”的实际工作，入口是--video-frame命令调用的ExportFirstVideoFrame。
// 它用libavformat逐包读取，再用libavcodec解码；第一张画面交EncodePng，后面的继续检查和计数。
// EncodePng先借助libswscale转换颜色，再用PNG编码器压成图片；PendingPng等整段读成功后才发布文件。
// 编解码器、转换器、帧包和临时文件各自负责释放，不依赖窗口存在；这里没有实时显示或播放时钟。
#define NOMINMAX
#include "VideoFrameExport.h"
#include "../ffmpeg/FfmpegSupport.h"
#include <filesystem>
extern "C" {
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace miniclip {
namespace {
struct CodecDelete {
    // 管理PNG编码器的智能指针会调用它，用avcodec_free_context归还资源，而不是直接delete。
    void operator()(AVCodecContext* p) const { avcodec_free_context(&p); }
};
struct ScaleDelete {
    // ConvertToRgb的智能指针通过sws_freeContext释放像素转换器，异常退出时也会执行。
    void operator()(SwsContext* p) const { sws_freeContext(p); }
};
using Codec = std::unique_ptr<AVCodecContext, CodecDelete>;
using Scale = std::unique_ptr<SwsContext, ScaleDelete>;

// 导出入口和解码循环在安全检查点调用它；发现原子停止标志后抛出取消异常，进入自动清理路径。
void CheckCancelled(const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) throw VideoExportCancelled();
}
// libavformat读取文件时用这个回调询问是否中断；它只读停止标志，不抛异常，也不能强杀任意底层操作。
int InterruptVideo(void* opaque) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(opaque);
    return cancel && cancel->load();
}

// ExportFirstVideoFrame用这个对象保管临时PNG，EncodePng只往里面写；整段成功后才改为最终文件名。
class PendingPng {
public:
    // 生成临时名称并用CREATE_NEW新建文件；名称已经存在就失败，不覆盖别人的文件。
    explicit PendingPng(const std::wstring& destination) : destination_(destination) {
        static std::atomic<unsigned long long> sequence{0};
        temporary_ = destination + L".part-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
        handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Create temporary PNG failed");
    }
    // 离开作用域时关闭句柄；图片还没正式发布就删除自己的临时文件，清理失败只记诊断，不再抛异常。
    ~PendingPng() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (!published_ && !DeleteFileW(temporary_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            std::fprintf(stderr, "PNG temporary cleanup failed: %lu\n", GetLastError());
    }
    // 临时文件只能由一个对象管理，禁止复制后重复关闭句柄或删除同一文件。
    PendingPng(const PendingPng&) = delete;
    // 赋值同样不能复制文件句柄和清理责任。
    PendingPng& operator=(const PendingPng&) = delete;
    // EncodePng取到编码包后调用它，用WriteFile写入图片字节；实际写入不足请求数量也按失败处理。
    void Write(const AVPacket* packet) {
        DWORD written = 0;
        if (packet->size <= 0 || !WriteFile(handle_, packet->data, static_cast<DWORD>(packet->size), &written, nullptr)
            || written != static_cast<DWORD>(packet->size))
            throw std::runtime_error("Write PNG packet failed");
    }
    // ExportFirstVideoFrame确认整段解码成功后调用它，先刷新并关闭临时文件，再以不覆盖方式改成最终名称。
    void Publish() {
        if (!FlushFileBuffers(handle_)) throw std::runtime_error("Flush PNG failed");
        if (!CloseHandle(handle_)) throw std::runtime_error("Close PNG failed");
        handle_ = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Publish PNG failed / 目标可能已存在");
        published_ = true;
    }
private:
    std::wstring destination_, temporary_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool published_ = false;
};

// 每张解码画面都会先经过这项检查：尺寸、像素存法必须在支持范围内，且不能有解码损伤。
// 当前只处理列出的8位SDR格式；HDR需要额外的色调映射，不能悄悄当普通图片保存。
void ValidatePicture(const AVFrame* frame) {
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 16384 || frame->height > 16384
        || int64_t{frame->width} * frame->height > 64000000)
        throw std::runtime_error("Unsupported video dimensions (limit: 64M pixels, each side <=16384)");
    switch (static_cast<AVPixelFormat>(frame->format)) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGR24: case AV_PIX_FMT_GRAY8: break;
    default: throw std::runtime_error("Unsupported pixel format / S02目前仅支持8位YUV420/RGB/BGR/灰度");
    }
    if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
        throw std::runtime_error("HDR tone mapping is not implemented");
    if (frame->decode_error_flags) throw std::runtime_error("Decoder reported damaged picture");
}

// ConvertToRgb根据画面声明的AVColorSpace选择swscale颜色矩阵，也就是YUV换算RGB时采用哪组系数。
// 没声明就用库默认，遇到不支持的矩阵则报错；这还不是完整的色彩管理系统。
int ColorMatrix(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
    case AVCOL_SPC_UNSPECIFIED: case AVCOL_SPC_RGB: return SWS_CS_DEFAULT;
    default: throw std::runtime_error("Unsupported color matrix");
    }
}

// EncodePng用它把首帧变成RGB24，也就是按红、绿、蓝三个字节保存每个像素的画面。
// 先为新帧分配存储，再配置sws_getContext和颜色矩阵/范围，最后由sws_scale执行转换并检查输出行数。
// 读取每个平面时使用真实linesize，即相邻两行的地址间隔，不假定行尾没有补齐字节。
Frame ConvertToRgb(const AVFrame* source) {
    Frame rgb = NewFrame();
    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = source->width;
    rgb->height = source->height;
    Check(av_frame_get_buffer(rgb.get(), 32), "Allocate RGB picture");
    Scale scale(sws_getContext(source->width, source->height, static_cast<AVPixelFormat>(source->format),
        rgb->width, rgb->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scale) throw std::runtime_error("Create pixel converter failed");
    const int* coefficients = sws_getCoefficients(ColorMatrix(source->colorspace));
    const int fullRange = source->color_range == AVCOL_RANGE_JPEG || source->format == AV_PIX_FMT_YUVJ420P;
    Check(sws_setColorspaceDetails(scale.get(), coefficients, fullRange, coefficients, 1,
        0, 1 << 16, 1 << 16), "Set picture color conversion");
    // 源图可能是多个YUV平面，目标RGB的行也可能对齐补齐，因此两端都必须传实际地址和linesize。
    const int rows = sws_scale(scale.get(), source->data, source->linesize, 0, source->height,
        rgb->data, rgb->linesize);
    if (rows != source->height) throw std::runtime_error("Incomplete RGB conversion");
    rgb->pts = 0; // 单张PNG独立编码，不把源视频时间戳当作PNG序号
    return rgb;
}

// 首张解码画面由它保存成真正的PNG，而不是把原始颜色数据换个后缀。
// 它先调用ConvertToRgb，再打开libavcodec的PNG编码器，通过send_frame/receive_packet送画面、取图片字节。
// 最后发送空帧通知编码器结束，确认只得到一个完整PNG包；包内容写入借用的PendingPng，发布留给外层。
void EncodePng(const AVFrame* source, PendingPng& output) {
    Frame rgb = ConvertToRgb(source);
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) throw std::runtime_error("PNG encoder unavailable");
    Codec encoder(avcodec_alloc_context3(codec));
    if (!encoder) throw std::bad_alloc();
    encoder->width = rgb->width;
    encoder->height = rgb->height;
    encoder->pix_fmt = AV_PIX_FMT_RGB24;
    encoder->time_base = AVRational{1, 1};
    Check(avcodec_open2(encoder.get(), codec, nullptr), "Open PNG encoder");
    Packet packet = NewPacket();
    int packets = 0;
    // 编码器一次可能暂时无包可取，因此循环receive_packet到EAGAIN或EOF；每个包写入后解除引用以便复用。
    auto receive = [&]() {
        for (;;) {
            int ret = avcodec_receive_packet(encoder.get(), packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return ret;
            Check(ret, "Receive PNG packet");
            output.Write(packet.get());
            ++packets;
            av_packet_unref(packet.get());
        }
    };
    Check(avcodec_send_frame(encoder.get(), rgb.get()), "Send PNG picture");
    receive();
    int ret = avcodec_send_frame(encoder.get(), nullptr);
    if (ret == AVERROR(EAGAIN)) { receive(); ret = avcodec_send_frame(encoder.get(), nullptr); }
    Check(ret, "Drain PNG encoder");
    if (receive() != AVERROR_EOF || packets != 1)
        throw std::runtime_error("PNG encoder did not produce exactly one complete image");
}
} // namespace

// --video-frame命令从这里进入S02。先检查输入和新PNG路径，再打开文件，挑第一条不是封面的真实视频轨。
// 逐包解码时首帧交EncodePng，其余画面继续校验和计数；文件结束后取完延迟画面，最后才Publish正式图片。
// 当前线程拥有输入、解码器、帧包及临时输出，任何一步失败或取消都会沿作用域清理，不能提前报告成功。
VideoFrameExportResult ExportFirstVideoFrame(const std::wstring& inputFile,
    const std::wstring& outputPng, const std::atomic<bool>* cancel) {
    CheckCancelled(cancel);
    if (!std::filesystem::is_regular_file(inputFile)) throw std::runtime_error("Input is not a local regular file");
    if (outputPng.empty() || _wcsicmp(std::filesystem::path(outputPng).extension().c_str(), L".png") != 0)
        throw std::runtime_error("Output must have .png extension");
    if (std::filesystem::exists(outputPng)) throw std::runtime_error("Output already exists / 不覆盖已有文件");
    // 先分配输入上下文、安装取消回调并限定file协议，再由open_input和find_stream_info读取文件和轨道信息。
    InputResources input;
    input.format = avformat_alloc_context();
    if (!input.format) throw std::bad_alloc();
    input.format->interrupt_callback = {InterruptVideo, const_cast<std::atomic<bool>*>(cancel)};
    Check(av_dict_set(&input.options, "protocol_whitelist", "file", 0), "Set video file protocol");
    int ret = avformat_open_input(&input.format, Utf8(std::filesystem::absolute(inputFile).wstring()).c_str(), nullptr, &input.options);
    CheckCancelled(cancel);
    Check(ret, "Open video file");
    ret = avformat_find_stream_info(input.format, nullptr);
    CheckCancelled(cancel);
    Check(ret, "Read video stream information");
    // 封面也可能登记成视频流，必须跳过；选定后按codec_id打开解码器，并先设置图像分配上限和损伤识别要求。
    VideoFrameExportResult result;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        const AVStream* stream = input.format->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            result.streamIndex = static_cast<int>(i);
            break;
        }
    }
    if (result.streamIndex < 0) throw std::runtime_error("No non-cover video stream / 没有可解码的视频轨道");
    const AVStream* stream = input.format->streams[result.streamIndex]; // 仅借用到input关闭
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("Video decoder unavailable");
    input.decoder = avcodec_alloc_context3(codec);
    if (!input.decoder) throw std::bad_alloc();
    Check(avcodec_parameters_to_context(input.decoder, stream->codecpar), "Set video decoder parameters");
    input.decoder->max_pixels = 64000000; // 尽早让解码器拒绝超范围图像，不只等分配后检查帧
    input.decoder->err_recognition = AV_EF_EXPLODE;
    Check(avcodec_open2(input.decoder, codec, nullptr), "Open video decoder");
    PendingPng output(outputPng);
    Frame frame = NewFrame();
    Packet packet = NewPacket();
    // 每次送包后调用这个接收函数，取完当前可用画面；首帧经EncodePng写临时图片，后续继续校验画面、检查尺寸并计数。
    auto receive = [&]() {
        for (;;) {
            CheckCancelled(cancel);
            int status = avcodec_receive_frame(input.decoder, frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return status;
            Check(status, "Decode video picture");
            ValidatePicture(frame.get());
            if (result.decodedFrames == 0) {
                result.width = frame->width;
                result.height = frame->height;
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    result.firstFrameSeconds = frame->best_effort_timestamp * av_q2d(stream->time_base);
                EncodePng(frame.get(), output);
            } else if (frame->width != result.width || frame->height != result.height) {
                throw std::runtime_error("Video dimensions changed / 动态尺寸尚未支持");
            }
            ++result.decodedFrames;
            av_frame_unref(frame.get());
        }
    };
    // avcodec_send_packet若返回EAGAIN，就先receive取走现成画面再重送同一包，不能把尚未接收的输入丢掉。
    auto send = [&](const AVPacket* value) {
        int status = avcodec_send_packet(input.decoder, value);
        if (status == AVERROR(EAGAIN)) { receive(); status = avcodec_send_packet(input.decoder, value); }
        Check(status, "Send video packet");
        return receive();
    };
    // read_frame会读到不同轨道的包，只把选中视频包交send解码；其他轨道跳过，每轮用unref清内容后复用包对象。
    for (;;) {
        CheckCancelled(cancel);
        ret = av_read_frame(input.format, packet.get());
        CheckCancelled(cancel);
        if (ret == AVERROR_EOF) break;
        if (ret == AVERROR(EAGAIN)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        Check(ret, "Read video packet");
        if (packet->stream_index == result.streamIndex) send(packet.get());
        av_packet_unref(packet.get());
    }
    // 文件读完后先检查I/O错误，再送空包取出B帧等延迟画面；有真实输出且没有取消，才允许Publish正式图片。
    if (input.format->pb && input.format->pb->error < 0) Check(input.format->pb->error, "Read video I/O");
    if (send(nullptr) != AVERROR_EOF) throw std::runtime_error("Video decoder did not finish draining");
    if (!result.decodedFrames) throw std::runtime_error("No video pictures decoded");
    CheckCancelled(cancel);
    output.Publish();
    result.outputFile = outputPng;
    return result;
}
}
