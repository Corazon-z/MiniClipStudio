// S05的视频生产线程从这里读取画面。libavcodec按源时间戳解码，libswscale转换成Win32可显示的BGRA。
// seek先从关键帧前置解码，保留覆盖目标时刻的画面，不把VFR素材强制改成固定帧率。
// 回调拿到的是自有像素值和代次；这里不访问窗口，调度线程再按音频时钟决定何时显示。
#define NOMINMAX
#include "VideoPreview.h"
#include "../ffmpeg/PreviewInput.h"
#include <limits>
extern "C" {
#include <libswscale/swscale.h>
}

namespace miniclip {
namespace {
constexpr AVRational Microseconds{1, AV_TIME_BASE};

struct ScaleDelete {
    // swscale缓存由当前解码线程拥有，正常或异常离开都会交给库释放。
    void operator()(SwsContext* context) const { sws_freeContext(context); }
};
using Scale = std::unique_ptr<SwsContext, ScaleDelete>;

// 每张帧都检查当前能解释的格式和尺寸；HDR必须另做色调映射，不能悄悄按普通SDR显示。
void ValidatePreviewPicture(const AVFrame* frame) {
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096 ||
        int64_t{frame->width} * frame->height > 16000000)
        throw std::runtime_error("Preview dimensions exceed 4096 per side or 16M pixels");
    switch (static_cast<AVPixelFormat>(frame->format)) {
    case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_RGB24: case AV_PIX_FMT_BGR24: case AV_PIX_FMT_GRAY8: break;
    default: throw std::runtime_error("Preview supports software 8-bit YUV420/RGB/BGR/gray pictures only");
    }
    if (frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
        throw std::runtime_error("HDR preview tone mapping is not implemented");
    if (frame->decode_error_flags) throw std::runtime_error("Decoder reported a damaged preview picture");
}

// 沿用S02的颜色矩阵选择；未声明时用swscale默认，不声称已经实现完整色彩管理。
int PreviewColorMatrix(AVColorSpace colorspace) {
    switch (colorspace) {
    case AVCOL_SPC_BT709: return SWS_CS_ITU709;
    case AVCOL_SPC_FCC: return SWS_CS_FCC;
    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL: return SWS_CS_BT2020;
    case AVCOL_SPC_UNSPECIFIED: case AVCOL_SPC_RGB: return SWS_CS_DEFAULT;
    default: throw std::runtime_error("Unsupported preview color matrix");
    }
}

// 时间戳换算后仍要检查范围，旧关键帧可能位于公共原点之前，不能用无符号数计算这些时间。
int64_t AddTime(int64_t left, int64_t right) {
    if ((right > 0 && left > (std::numeric_limits<int64_t>::max)() - right) ||
        (right < 0 && left < (std::numeric_limits<int64_t>::min)() - right))
        throw std::runtime_error("Preview video time is out of range");
    return left + right;
}

// 尾帧没有下一帧可相减，优先用解码帧携带的duration，再用流帧率估计；未知不伪装成0秒。
int64_t FinalDuration(const AVFrame* frame, const AVStream* stream, const AVCodecContext* decoder) {
    if (frame->duration > 0) {
        const int64_t duration = av_rescale_q(frame->duration, stream->time_base, Microseconds);
        if (duration > 0) return duration;
    }
    AVRational rate = stream->avg_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) rate = stream->r_frame_rate;
    if (rate.num <= 0 || rate.den <= 0) rate = decoder->framerate;
    if (rate.num <= 0 || rate.den <= 0) throw std::runtime_error("Final preview picture has no duration or usable frame rate");
    const int64_t duration = av_rescale_q(1, AVRational{rate.den, rate.num}, Microseconds);
    if (duration <= 0) throw std::runtime_error("Invalid final preview picture duration");
    return duration;
}

// 只有会交付的帧才做颜色转换，seek前置解码的旧帧无需分配BGRA。行地址按实际linesize读取。
PreviewPicture ConvertPreviewPicture(const AVFrame* frame, Scale& scale, int64_t ptsUs, int64_t durationUs,
    uint64_t generation) {
    const AVPixelFormat format = static_cast<AVPixelFormat>(frame->format);
    SwsContext* updated = sws_getCachedContext(scale.release(), frame->width, frame->height, format,
        frame->width, frame->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    scale.reset(updated);
    if (!scale) throw std::runtime_error("Create preview pixel converter failed");
    const int* coefficients = sws_getCoefficients(PreviewColorMatrix(frame->colorspace));
    const int fullRange = frame->color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P;
    Check(sws_setColorspaceDetails(scale.get(), coefficients, fullRange, coefficients, 1,
        0, 1 << 16, 1 << 16), "Set preview color conversion");
    PreviewPicture picture;
    picture.generation = generation; picture.ptsUs = ptsUs; picture.durationUs = durationUs;
    picture.width = frame->width; picture.height = frame->height;
    picture.bgra.resize(static_cast<size_t>(frame->width) * frame->height * 4);
    uint8_t* output[] = {picture.bgra.data(), nullptr, nullptr, nullptr};
    const int lines[] = {frame->width * 4, 0, 0, 0};
    const int rows = sws_scale(scale.get(), frame->data, frame->linesize, 0, frame->height, output, lines);
    if (rows != frame->height) throw std::runtime_error("Incomplete preview pixel conversion");
    return picture;
}
}

// 每代都从独立输入开始，避免旧seek留下的参考帧或EOF状态进入下一代。
// 持有前一帧等下一PTS，便可确定它覆盖的时间区间；只有区间还没结束于目标之前的帧才交给会话。
PreviewDecodeResult DecodePreviewVideo(const PreviewSource& source, int64_t targetUs,
    PreviewReadControl control, const std::function<void(PreviewPicture)>& consume) {
    control.Check();
    if (targetUs < 0 || source.videoStream < 0 || !consume)
        throw std::invalid_argument("Invalid preview video request");
    if (source.originUs == (std::numeric_limits<int64_t>::min)())
        throw std::runtime_error("Invalid preview source origin");
    PreviewInput input(source.path, control, source.videoStream);
    const AVStream* stream = input.Stream();
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw std::runtime_error("Selected preview stream is not video");
    if (targetUs > 0) input.Seek(AddTime(source.originUs, targetUs));
    Frame decoded = NewFrame();
    Frame pending = NewFrame();
    Packet packet = NewPacket();
    Scale scale;
    PreviewDecodeResult result;
    bool hasPending = false;
    int firstWidth = 0, firstHeight = 0;
    int64_t pendingPts = 0;
    int64_t lastSourcePts = AV_NOPTS_VALUE;

    // 前一帧的区间在target处尚未结束，就保留它作为seek首图；后面的帧照源PTS顺序交付。
    auto deliver = [&](int64_t durationUs, bool finalFrame = false) {
        control.Check();
        if (durationUs <= 0) throw std::runtime_error("Non-positive preview picture duration");
        const int64_t endUs = AddTime(pendingPts, durationUs);
        result.endUs = std::max(result.endUs, endUs);
        if (endUs <= targetUs && !finalFrame) return;
        auto picture = ConvertPreviewPicture(pending.get(), scale, pendingPts, durationUs, control.generation);
        picture.finalFrame = finalFrame;
        control.Check();
        consume(std::move(picture));
        ++result.frames;
    };

    // 一包可能解出多帧；实际PTS决定显示顺序，不能把包DTS或frameIndex/fps当成画面的播放时刻。
    auto receive = [&]() {
        for (;;) {
            control.Check();
            const int code = avcodec_receive_frame(input.Decoder(), decoded.get());
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return code;
            input.CheckResult(code, "Receive preview video picture");
            ValidatePreviewPicture(decoded.get());
            const int64_t timestamp = decoded->best_effort_timestamp;
            if (timestamp == AV_NOPTS_VALUE) throw std::runtime_error("Missing preview video timestamp");
            if (lastSourcePts != AV_NOPTS_VALUE && timestamp <= lastSourcePts)
                throw std::runtime_error("Non-increasing preview video timestamp");
            lastSourcePts = timestamp;
            const int64_t absoluteUs = av_rescale_q(timestamp, stream->time_base, Microseconds);
            if (absoluteUs == AV_NOPTS_VALUE) throw std::runtime_error("Preview video timestamp overflow");
            const int64_t ptsUs = AddTime(absoluteUs, -source.originUs);
            if (ptsUs == AV_NOPTS_VALUE) throw std::runtime_error("Preview relative video timestamp overflow");
            if (!hasPending) { firstWidth = decoded->width; firstHeight = decoded->height; }
            else {
                if (decoded->width != firstWidth || decoded->height != firstHeight)
                    throw std::runtime_error("Video dimensions changed during preview");
                if (ptsUs <= pendingPts) throw std::runtime_error("Preview video timestamps collapse in microseconds");
                deliver(AddTime(ptsUs, -pendingPts));
                av_frame_unref(pending.get());
            }
            av_frame_move_ref(pending.get(), decoded.get());
            pendingPts = ptsUs;
            hasPending = true;
        }
    };

    // 解码器暂时不接收包时先取结果再重送；空包代表结束，返回值用于确认尾部真的排到EOF。
    auto send = [&](const AVPacket* value) {
        control.Check();
        int code = avcodec_send_packet(input.Decoder(), value);
        if (code == AVERROR(EAGAIN)) { receive(); code = avcodec_send_packet(input.Decoder(), value); }
        input.CheckResult(code, "Send preview video packet");
        return receive();
    };

    for (;;) {
        control.Check();
        const int code = input.Read(packet.get());
        if (code == AVERROR_EOF) break;
        if (code == AVERROR(EAGAIN)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        input.CheckResult(code, "Read preview video packet");
        if (packet->stream_index == source.videoStream) send(packet.get());
        av_packet_unref(packet.get());
    }
    if (input.Format()->pb) input.CheckResult(input.Format()->pb->error, "Read preview video IO");
    if (send(nullptr) != AVERROR_EOF) throw std::runtime_error("Preview video decoder did not finish draining");
    if (!hasPending) throw std::runtime_error("No preview video pictures decoded");
    // 目标在视频末尾之后时仍交出重新解码的末图，让仍有声音的尾段保持画面，不借用上一代旧图。
    deliver(FinalDuration(pending.get(), stream, input.Decoder()), true);
    control.Check();
    return result;
}
}
