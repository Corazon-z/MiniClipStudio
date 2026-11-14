// S05的音频和视频各自在自己的线程打开文件，复用这里的本地输入、解码器、seek和中断处理。
// VideoPlayer先用ReadPreviewSource取得所选轨道与共同时间原点，再让两个解码线程分别创建PreviewInput。
// FFmpeg上下文不跨线程共享；停止或新一代seek到来时抛PreviewInterrupted，由会话统一回收旧工作。
#define NOMINMAX
#include "PreviewInput.h"
#include <filesystem>
#include <limits>

namespace miniclip {
namespace {
constexpr auto IoTimeout = std::chrono::seconds(10);
constexpr AVRational Microseconds{1, AV_TIME_BASE};

// 将轨道的时间格数换成微秒；无效时间单位或溢出不能继续参与共同时间轴计算。
int64_t ToMicroseconds(int64_t value, AVRational timeBase) {
    if (timeBase.num <= 0 || timeBase.den <= 0)
        throw std::runtime_error("Invalid preview stream time base");
    const int64_t result = av_rescale_q(value, timeBase, Microseconds);
    if (result == AV_NOPTS_VALUE) throw std::runtime_error("Preview timestamp is out of range");
    return result;
}

// 源时间可能不是从0开始，先检查相加/相减是否越界，避免坏元数据触发有符号整数溢出。
int64_t AddTime(int64_t left, int64_t right) {
    if ((right > 0 && left > (std::numeric_limits<int64_t>::max)() - right) ||
        (right < 0 && left < (std::numeric_limits<int64_t>::min)() - right))
        throw std::runtime_error("Preview time calculation is out of range");
    return left + right;
}
}

// 所有同步解码步骤都先询问会话是否仍需要本代结果；新seek只取消旧代，不应被报告成媒体故障。
void PreviewReadControl::Check() const {
    if (Interrupted()) throw PreviewInterrupted();
}

// 先限定本地文件并探测轨道；指定轨道时再按实际codec_id准备解码器，不根据扩展名猜编码。
// 每次输入IO都有独立10秒协作式期限；构造失败时成员InputResources仍会释放已取得的资源。
PreviewInput::PreviewInput(const std::wstring& file, PreviewReadControl control, int streamIndex)
    : control_(control), deadline_(std::chrono::steady_clock::now() + IoTimeout), streamIndex_(streamIndex) {
    control_.Check();
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error))
        throw std::runtime_error("Preview requires an existing local file / 请选择已有本地文件");
    if (streamIndex < -1) throw std::invalid_argument("Invalid preview stream index");
    resources_.format = avformat_alloc_context();
    if (!resources_.format) throw std::bad_alloc();
    resources_.format->interrupt_callback = {Interrupt, this};
    Check(av_dict_set(&resources_.options, "protocol_whitelist", "file", 0), "Restrict preview to local files");
    const std::string path = Utf8(std::filesystem::absolute(file).wstring());
    deadline_ = std::chrono::steady_clock::now() + IoTimeout;
    int result = avformat_open_input(&resources_.format, path.c_str(), nullptr, &resources_.options);
    CheckIoResult(result, "Open preview input");
    deadline_ = std::chrono::steady_clock::now() + IoTimeout;
    result = avformat_find_stream_info(resources_.format, nullptr);
    CheckIoResult(result, "Read preview stream information");
    if (streamIndex_ < 0) return;
    if (static_cast<unsigned>(streamIndex_) >= resources_.format->nb_streams)
        throw std::runtime_error("Selected preview stream no longer exists");

    // 后续只需本轨数据；其他轨道不影响这份独立输入的解码与定位。
    for (unsigned i = 0; i < resources_.format->nb_streams; ++i)
        resources_.format->streams[i]->discard = static_cast<int>(i) == streamIndex_ ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    const AVStream* stream = Stream();
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0)
        throw std::runtime_error("Invalid selected preview stream time base");
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("Preview decoder unavailable");
    resources_.decoder = avcodec_alloc_context3(codec);
    if (!resources_.decoder) throw std::bad_alloc();
    CheckResult(avcodec_parameters_to_context(resources_.decoder, stream->codecpar), "Set preview decoder parameters");
    resources_.decoder->pkt_timebase = stream->time_base;
    resources_.decoder->thread_count = 1;
    resources_.decoder->max_pixels = 16000000;
    resources_.decoder->err_recognition = AV_EF_EXPLODE;
    CheckResult(avcodec_open2(resources_.decoder, codec, nullptr), "Open preview decoder");
}

// 供当前线程借用选中的流；只探测模式没有选中轨道，不能误当第0轨使用。
const AVStream* PreviewInput::Stream() const {
    if (streamIndex_ < 0) throw std::logic_error("Preview input has no selected stream");
    return resources_.format->streams[streamIndex_];
}

// FFmpeg在IO内部反复查询这个回调，期限和上下文只由本线程改动，跨线程请求则用atomic读取。
int PreviewInput::Interrupt(void* opaque) {
    const auto* self = static_cast<const PreviewInput*>(opaque);
    return self->control_.Interrupted() || std::chrono::steady_clock::now() >= self->deadline_;
}

// 先识别旧代取消，随后才解释真正的FFmpeg错误；普通解码检查不受上一次输入IO期限影响。
void PreviewInput::CheckResult(int code, const char* stage) const {
    control_.Check();
    Check(code, stage);
}

// 输入操作结束后额外检查期限，连“返回成功但已超时”也不忽略；这仍依靠底层配合，不能强制中止任意系统调用。
void PreviewInput::CheckIoResult(int code, const char* stage) const {
    control_.Check();
    if (std::chrono::steady_clock::now() >= deadline_)
        throw std::runtime_error(std::string(stage) + ": input IO timed out / 读取媒体超过10秒");
    Check(code, stage);
}

// GUI给出的目标是微秒；向下换成轨道时间格数后，回到不晚于目标的关键帧，再清掉解码器旧参考状态。
// seek只负责回到可解码位置，调用方仍必须解出关键帧之后的画面或样本，丢弃目标之前的内容。
void PreviewInput::Seek(int64_t absoluteUs) {
    control_.Check();
    const AVStream* stream = Stream();
    const int64_t timestamp = av_rescale_q_rnd(absoluteUs, Microseconds, stream->time_base, AV_ROUND_DOWN);
    if (timestamp == AV_NOPTS_VALUE) throw std::runtime_error("Preview seek time is out of range");
    deadline_ = std::chrono::steady_clock::now() + IoTimeout;
    const int result = av_seek_frame(resources_.format, streamIndex_, timestamp, AVSEEK_FLAG_BACKWARD);
    CheckIoResult(result, "Seek preview input to preceding keyframe");
    avcodec_flush_buffers(resources_.decoder);
    control_.Check();
}

// 每次读包重新给10秒期限；EOF和暂时无包保留为状态值，其他读取失败立即带阶段信息抛出。
int PreviewInput::Read(AVPacket* packet) {
    control_.Check();
    if (!packet) throw std::invalid_argument("Preview read requires a packet");
    deadline_ = std::chrono::steady_clock::now() + IoTimeout;
    const int result = av_read_frame(resources_.format, packet);
    if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
        CheckIoResult(0, "Read preview packet");
        return result;
    }
    CheckIoResult(result, "Read preview packet");
    return result;
}

// 打开预览前先复制首条非封面视频和可选首音轨；两轨保留同一个原点，不能各减自己的首PTS。
// 时长优先使用所有选中轨道的最晚结束；某轨未提供时，退回容器时长，再未知就留空。
PreviewSource ReadPreviewSource(const std::wstring& file, PreviewReadControl control) {
    PreviewInput input(file, control);
    const AVFormatContext* format = input.Format();
    PreviewSource source;
    source.path = std::filesystem::absolute(file).wstring();
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        if (source.videoStream < 0 && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) source.videoStream = static_cast<int>(i);
        if (source.audioStream < 0 && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            source.audioStream = static_cast<int>(i);
    }
    if (source.videoStream < 0) throw std::runtime_error("Preview requires a non-cover video stream / 文件中没有可预览的视频轨");
    const AVStream* video = format->streams[source.videoStream];
    const AVStream* audio = source.audioStream >= 0 ? format->streams[source.audioStream] : nullptr;
    if (format->start_time != AV_NOPTS_VALUE) source.originUs = format->start_time;
    else {
        bool found = false;
        for (const AVStream* stream : {video, audio}) {
            if (!stream || stream->start_time == AV_NOPTS_VALUE) continue;
            const int64_t start = ToMicroseconds(stream->start_time, stream->time_base);
            if (!found || start < source.originUs) source.originUs = start;
            found = true;
        }
    }
    bool allDurationsKnown = true;
    int64_t lastEnd = 0;
    for (const AVStream* stream : {video, audio}) {
        if (!stream) continue;
        if (stream->duration == AV_NOPTS_VALUE || stream->duration <= 0) { allDurationsKnown = false; continue; }
        const int64_t start = stream->start_time == AV_NOPTS_VALUE ? source.originUs :
            ToMicroseconds(stream->start_time, stream->time_base);
        const int64_t end = AddTime(start, ToMicroseconds(stream->duration, stream->time_base));
        if (source.originUs == (std::numeric_limits<int64_t>::min)())
            throw std::runtime_error("Invalid preview time origin");
        const int64_t relativeEnd = AddTime(end, -source.originUs);
        if (relativeEnd > lastEnd) lastEnd = relativeEnd;
    }
    if (allDurationsKnown && lastEnd > 0) source.durationUs = lastEnd;
    else if (format->duration != AV_NOPTS_VALUE && format->duration > 0) source.durationUs = format->duration;
    control.Check();
    return source;
}
}
