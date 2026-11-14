// S05的音频生产线程每次代次都重新进入这里，单独打开输入并前置解码到目标位置。
// libavcodec还原声音，libswresample统一为48k双声道float；每块同时带媒体采样位置和generation，
// 消费端据此保留首音轨偏移、补前导静音，并拒绝跳转前的旧结果。S04的DecodeAudioFile不经这里。
#define NOMINMAX
#include "AudioPreview.h"
#include "../ffmpeg/PreviewInput.h"
#include <algorithm>
#include <cstdlib>
#include <limits>

namespace miniclip {
namespace {
// 一次代次独占转换器与复制的源布局。跳转中断直接析构，不把旧滤波器尾部排进新位置。
struct PreviewResampler {
    SwrContext* context = nullptr;
    AVChannelLayout layout{};
    int rate = 0;
    int format = AV_SAMPLE_FMT_NONE;
    // 先保持空状态，等首帧决定真实源格式之后才建立swr。
    PreviewResampler() = default;
    // swr和源布局只能归这个代次所有，不能复制成两份释放责任。
    PreviewResampler(const PreviewResampler&) = delete;
    // 赋值也不能复制转换器及布局所有权。
    PreviewResampler& operator=(const PreviewResampler&) = delete;
    // 正常EOF或异常/跳转离开时都归还转换资源；只有正常EOF才在外层显式排空。
    ~PreviewResampler() { swr_free(&context); av_channel_layout_uninit(&layout); }
};
// FFmpeg返回无时间或换算溢出的标记不能变成音画同步锚点，统一在建立时间位置时拒绝。
int64_t RescalePosition(int64_t value, AVRational from, AVRational to) {
    const int64_t result = av_rescale_q(value, from, to);
    if (result == std::numeric_limits<int64_t>::min()) throw std::runtime_error("Audio preview timestamp overflow");
    return result;
}
// 将首帧相对共同文件原点的时间转换到48k采样格；用差值防止两条轨道各自从首帧归零。
int64_t RelativeSample(int64_t timestamp, AVRational timeBase, int64_t originUs) {
    const int64_t absoluteUs = RescalePosition(timestamp, timeBase, AV_TIME_BASE_Q);
    if ((originUs > 0 && absoluteUs < std::numeric_limits<int64_t>::min() + originUs) ||
        (originUs < 0 && absoluteUs > std::numeric_limits<int64_t>::max() + originUs))
        throw std::runtime_error("Audio preview relative timestamp overflow");
    return RescalePosition(absoluteUs - originUs, AV_TIME_BASE_Q, AVRational{1, PreviewRate});
}
}

// VideoPlayer的音频生产线程同步调用本入口，先seek，再按真实帧建立新的swr，避免沿用旧代次延迟。
// 首帧PTS确定共同媒体采样锚点，之后只按swr真实输出数量前进；跨目标块按采样点裁头，正常EOF保留尾块。
PreviewDecodeResult DecodePreviewAudio(const PreviewSource& source, int64_t targetUs,
    PreviewReadControl control, const std::function<void(PreviewAudioBlock)>& consume) {
    control.Check();
    if (source.audioStream < 0 || targetUs < 0 || !consume) throw std::invalid_argument("Invalid audio preview request");
    if (source.originUs > std::numeric_limits<int64_t>::max() - targetUs)
        throw std::runtime_error("Audio preview seek target overflow");
    PreviewInput input(source.path, control, source.audioStream);
    input.Seek(source.originUs + targetUs);
    const AVStream* stream = input.Stream();
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0) throw std::runtime_error("Invalid audio preview time base");
    const int64_t targetSample = av_rescale_q_rnd(targetUs, AV_TIME_BASE_Q,
        AVRational{1, PreviewRate}, AV_ROUND_UP);
    if (targetSample < 0) throw std::runtime_error("Audio preview target sample overflow");
    PreviewDecodeResult result;
    result.endUs = targetUs; // 目标已在音轨末尾之后时没有样本，消费端仍可输出静音直到视频结束。
    PreviewResampler resampler;
    Frame frame = NewFrame();
    Packet packet = NewPacket();
    std::vector<float> converted;
    PreviewAudioBlock block;
    block.generation = control.generation;
    bool haveTime = false;
    int64_t nextSourceSample = 0;
    int64_t nextOutputSample = 0;

    // receive或swr排空每产生一段样本就调用deliver，保持绝对采样位置，跳过目标之前的数据。
    // 块只交出有效frames部分；每次consume前检查代次，回调自身也须在等待/入队时检查相同请求。
    auto deliver = [&](int produced) {
        if (nextOutputSample > std::numeric_limits<int64_t>::max() - produced)
            throw std::runtime_error("Audio preview output position overflow");
        const int64_t endSample = nextOutputSample + produced;
        int offset = 0;
        if (endSample <= targetSample) offset = produced;
        else if (nextOutputSample < targetSample)
            offset = static_cast<int>(targetSample - nextOutputSample);
        while (offset < produced) {
            control.Check();
            if (block.frames == 0) block.startSample = nextOutputSample + offset;
            const int count = std::min(PreviewBlockFrames - block.frames, produced - offset);
            std::copy_n(converted.data() + offset * PreviewChannels, count * PreviewChannels,
                block.samples.data() + block.frames * PreviewChannels);
            offset += count;
            block.frames += count;
            result.frames += count;
            if (block.frames == PreviewBlockFrames) {
                control.Check();
                consume(std::move(block));
                block = PreviewAudioBlock{};
                block.generation = control.generation;
            }
        }
        nextOutputSample = endSample;
        result.endUs = RescalePosition(endSample, AVRational{1, PreviewRate}, AV_TIME_BASE_Q);
    };
    // 每次交包后取完解码帧，先核对时间/格式，再调用swr_convert；不能以编码包大小推导样本数。
    auto receive = [&]() {
        for (;;) {
            control.Check();
            const int code = avcodec_receive_frame(input.Decoder(), frame.get());
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return;
            input.CheckResult(code, "Decode preview audio");
            if (frame->sample_rate < 8000 || frame->sample_rate > 192000 ||
                frame->ch_layout.nb_channels < 1 || frame->ch_layout.nb_channels > 2 ||
                !av_channel_layout_check(&frame->ch_layout) || frame->nb_samples <= 0 || frame->nb_samples > 65536)
                throw std::runtime_error("Preview audio requires fixed mono/stereo, 8–192kHz and bounded frames");
            if (frame->best_effort_timestamp == AV_NOPTS_VALUE)
                throw std::runtime_error("Preview audio frame has no usable timestamp");
            if (resampler.context && (resampler.rate != frame->sample_rate || resampler.format != frame->format ||
                av_channel_layout_compare(&resampler.layout, &frame->ch_layout) != 0))
                throw std::runtime_error("Preview audio format changed inside the selected stream");
            const int64_t sourceSample = RescalePosition(frame->best_effort_timestamp,
                stream->time_base, AVRational{1, frame->sample_rate});
            if (!haveTime) {
                nextSourceSample = sourceSample;
                nextOutputSample = RelativeSample(frame->best_effort_timestamp, stream->time_base, source.originUs);
                haveTime = true;
            }
            // 容器可能把PTS按毫秒保存，允许一个timestamp刻度加2样本的舍入误差，但不自动补中间时间洞。
            const int64_t tickSamples = av_rescale_q_rnd(1, stream->time_base,
                AVRational{1, frame->sample_rate}, AV_ROUND_UP);
            if (tickSamples > frame->sample_rate / 100)
                throw std::runtime_error("Preview audio time base is too coarse to verify continuous samples");
            const int64_t tolerance = std::max<int64_t>(2, tickSamples + 2);
            const int64_t minimumAllowed = nextSourceSample < std::numeric_limits<int64_t>::min() + tolerance
                ? std::numeric_limits<int64_t>::min() : nextSourceSample - tolerance;
            const int64_t maximumAllowed = nextSourceSample > std::numeric_limits<int64_t>::max() - tolerance
                ? std::numeric_limits<int64_t>::max() : nextSourceSample + tolerance;
            if (sourceSample < minimumAllowed || sourceSample > maximumAllowed)
                throw std::runtime_error("Preview audio timestamps contain an unsupported gap or overlap");
            if (nextSourceSample > std::numeric_limits<int64_t>::max() - frame->nb_samples)
                throw std::runtime_error("Audio preview source position overflow");
            nextSourceSample += frame->nb_samples;

            if (!resampler.context) {
                resampler.rate = frame->sample_rate;
                resampler.format = frame->format;
                input.CheckResult(av_channel_layout_copy(&resampler.layout, &frame->ch_layout), "Copy preview audio layout");
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                input.CheckResult(swr_alloc_set_opts2(&resampler.context, &stereo, AV_SAMPLE_FMT_FLT, PreviewRate,
                    &resampler.layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr),
                    "Allocate preview resampler");
                input.CheckResult(swr_init(resampler.context), "Initialize preview resampler");
            }
            const int64_t capacity = av_rescale_rnd(swr_get_delay(resampler.context, resampler.rate) + frame->nb_samples,
                PreviewRate, resampler.rate, AV_ROUND_UP);
            if (capacity <= 0 || capacity > 500000) throw std::runtime_error("Preview audio conversion buffer limit exceeded");
            converted.resize(static_cast<size_t>(capacity) * PreviewChannels);
            uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
            const int produced = swr_convert(resampler.context, output, static_cast<int>(capacity),
                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            input.CheckResult(produced, "Convert preview audio");
            deliver(produced);
            av_frame_unref(frame.get());
        }
    };
    // send_packet遇EAGAIN先取帧再重送同一包，空包则通知解码器结束，取出正常EOF的尾部。
    auto send = [&](const AVPacket* value) {
        int code = avcodec_send_packet(input.Decoder(), value);
        if (code == AVERROR(EAGAIN)) { receive(); code = avcodec_send_packet(input.Decoder(), value); }
        input.CheckResult(code, "Send preview audio packet");
        receive();
    };
    for (;;) {
        control.Check();
        const int code = input.Read(packet.get());
        if (code == AVERROR_EOF) break;
        input.CheckResult(code, "Read preview audio packet");
        if (packet->stream_index == source.audioStream) send(packet.get());
        av_packet_unref(packet.get());
    }
    control.Check();
    send(nullptr);
    if (resampler.context) {
        converted.resize(PreviewBlockFrames * PreviewChannels);
        for (;;) {
            control.Check();
            uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
            const int produced = swr_convert(resampler.context, output, PreviewBlockFrames, nullptr, 0);
            input.CheckResult(produced, "Drain preview resampler");
            if (!produced) break;
            deliver(produced);
        }
    }
    if (block.frames) { control.Check(); consume(std::move(block)); }
    control.Check();
    if (result.frames == 0) result.endUs = targetUs;
    return result;
}
}
