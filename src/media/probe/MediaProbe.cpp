// S01在这里读取媒体文件的容器、音视频轨道和编码参数，再把这些“文件说明”整理成报告。
// 窗口后台任务和CLI共用ProbeMediaFile；它用libavformat的open_input/find_stream_info打开并识别文件，
// 再由ReadStreamInformation复制每轨参数。调用方拿到MediaInfo后，另外调用FormatMediaInfo显示文字。
// FFmpeg输入只在本次调用中存在；报告保存的是复制的数据，不依赖文件保持打开，也不代表已完整解码。
#include "MediaProbe.h"
#include "../ffmpeg/FfmpegSupport.h"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
extern "C" {
#include <libavutil/pixdesc.h>
}
namespace miniclip {
namespace {
struct ProbeReadState {
    const std::atomic<bool>* cancel;
    std::chrono::steady_clock::time_point deadline;
};
// FFmpeg读文件时会通过这个中断回调询问是否继续；它检查停止标志和期限，只返回判断，不抛异常或释放资源。
int InterruptProbe(void* opaque) {
    const auto* state = static_cast<const ProbeReadState*>(opaque);
    return (state->cancel && state->cancel->load()) ||
        std::chrono::steady_clock::now() >= state->deadline;
}
// ProbeMediaFile在耗时操作前后调用它，把“用户叫停”和“超过期限”转换成不同异常，避免笼统报成读取失败。
void CheckProbeState(const ProbeReadState& state) {
    if (state.cancel && state.cancel->load()) throw MediaProbeCancelled();
    if (std::chrono::steady_clock::now() >= state.deadline)
        throw std::runtime_error("Media probe timed out / 媒体探测超时");
}
// 容器和轨道时长都通过它换算：duration是时间格数，time_base是每格几秒，相乘便得到秒数。
// 文件未提供时长或单位无效时返回nullopt表示未知，不能用0秒代替。
std::optional<double> ReadDuration(int64_t duration, AVRational unit) {
    if (duration == AV_NOPTS_VALUE || duration < 0 || unit.num <= 0 || unit.den <= 0) return std::nullopt;
    return duration * av_q2d(unit);
}
// 探测结果只采信大于0的码率，也就是每秒比特数；0或负值按“未提供”处理，保留为可选值。
std::optional<int64_t> ReadBitRate(int64_t value) {
    return value > 0 ? std::optional<int64_t>(value) : std::nullopt;
}
// ReadStreamInformation用它复制有理数的分子分母，保留时间单位和帧率的原始精度，不在这里猜未知值。
MediaRational CopyRational(AVRational value) { return {value.num, value.den}; }
// ProbeMediaFile为每条轨道调用它，从AVStream及其codecpar中抄出编码、时长、格式等参数。
// 音轨再取采样率/声道，视频轨再取尺寸/帧率；返回自己的字符串和数字，不把库内地址带出文件会话。
MediaStreamInfo ReadStreamInformation(const AVStream* stream) {
    MediaStreamInfo info;
    const AVCodecParameters* parameters = stream->codecpar; // 借用，不单独释放
    info.index = stream->index;
    const char* type = av_get_media_type_string(parameters->codec_type);
    info.type = type ? type : "unknown";
    info.codecName = parameters->codec_id == AV_CODEC_ID_NONE ? "unknown" : avcodec_get_name(parameters->codec_id);
    const AVCodecDescriptor* description = avcodec_descriptor_get(parameters->codec_id);
    if (description && description->long_name) info.codecDescription = description->long_name;
    info.timeBase = CopyRational(stream->time_base);
    info.durationSeconds = ReadDuration(stream->duration, stream->time_base);
    info.bitRate = ReadBitRate(parameters->bit_rate);
    const AVDictionaryEntry* language = av_dict_get(stream->metadata, "language", nullptr, 0);
    if (language) info.language = language->value;
    // 音轨读取采样率/声道布局/样本格式；画面轨读取尺寸/像素格式/帧率/封面标记。
    if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (parameters->sample_rate > 0) info.sampleRate = parameters->sample_rate;
        if (parameters->ch_layout.nb_channels > 0) {
            info.channels = parameters->ch_layout.nb_channels;
            char layout[256]{};
            Check(av_channel_layout_describe(&parameters->ch_layout, layout, sizeof(layout)), "Describe audio layout");
            info.channelLayout = layout;
        }
        const char* format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(parameters->format));
        if (format) info.sampleFormat = format;
    } else if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
        if (parameters->width > 0) info.width = parameters->width;
        if (parameters->height > 0) info.height = parameters->height;
        const char* format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(parameters->format));
        if (format) info.pixelFormat = format;
        info.averageFrameRate = CopyRational(stream->avg_frame_rate);
        info.referenceFrameRate = CopyRational(stream->r_frame_rate);
        info.attachedPicture = (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
    }
    return info;
}
// FormatMediaInfo显示名称时调用它，空字符串统一写成“未知”，避免把缺失信息显示为空白。
std::string KnownText(const std::string& value) { return value.empty() ? "未知" : value; }
// 报告中的采样率、尺寸等可选整数由它转成文字；有数值就显示，没有就写“未知”，与数字0分开。
std::string Number(const std::optional<int>& value) { return value ? std::to_string(*value) : "未知"; }
// FormatMediaInfo用它给码率加bit/s单位；这里只排文字，不执行音频重采样或修改文件。
std::string Rate(const std::optional<int64_t>& value) { return value ? std::to_string(*value) + " bit/s" : "未知"; }
// 把报告中的时长统一显示为秒并保留6位小数；文件没提供时长时仍写“未知”。
std::string Duration(const std::optional<double>& value) {
    if (!value) return "未知";
    std::ostringstream text;
    text << std::fixed << std::setprecision(6) << *value << " s";
    return text.str();
}
// 报告用它显示时间单位和帧率：先确认分子分母有效，再保留分数；帧率还额外显示每秒多少帧的近似值。
std::string Rational(MediaRational value, bool frameRate) {
    if (value.numerator <= 0 || value.denominator <= 0) return "未知";
    std::ostringstream text;
    text << value.numerator << '/' << value.denominator;
    if (frameRate) text << " (" << std::fixed << std::setprecision(3)
        << static_cast<double>(value.numerator) / value.denominator << " frame/s)";
    return text.str();
}
} // 内部辅助函数不暴露给其他文件
// 窗口后台任务或--probe命令从这里进入S01。先检查停止请求和本地路径，再用libavformat打开并识别文件。
// 容器信息直接复制，每条轨道交ReadStreamInformation读取；返回前InputResources自动关闭文件，报告仍然有效。
// 中断回调和阶段后的CheckProbeState共同区分取消、超时与真正的媒体读取错误。
MediaInfo ProbeMediaFile(const std::wstring& path, const std::atomic<bool>* cancel) {
    ProbeReadState state{cancel, std::chrono::steady_clock::now() + std::chrono::seconds(15)};
    CheckProbeState(state);
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(path, fileError))
        throw std::runtime_error("Input is not a readable local regular file / 找不到本地普通文件");
    // 绝对文件路径不会被误当成网络URL；本阶段不探测网络流。
    const std::string inputPath = Utf8(std::filesystem::absolute(path).wstring());
    // 先准备输入上下文并安装中断回调，再限制file协议；接下来的open_input和find_stream_info才真正读取媒体。
    InputResources input;
    input.format = avformat_alloc_context();
    if (!input.format) throw std::bad_alloc();
    input.format->interrupt_callback = {InterruptProbe, &state}; // state比input先创建、后销毁
    Check(av_dict_set(&input.options, "protocol_whitelist", "file", 0), "Set local-file protocol");
    int result = avformat_open_input(&input.format, inputPath.c_str(), nullptr, &input.options);
    CheckProbeState(state);
    Check(result, "Open media file / 打开媒体文件");
    result = avformat_find_stream_info(input.format, nullptr);
    CheckProbeState(state);
    Check(result, "Read stream information / 识别媒体轨道");
    if (!input.format->nb_streams) throw std::runtime_error("No media streams / 没有可识别的媒体轨道");
    // 先复制容器总信息，再逐轨读取。字符串都抄到自己的对象里，不返回AVStream或字典条目的借用指针。
    MediaInfo info;
    info.path = inputPath;
    if (input.format->iformat->name) info.containerName = input.format->iformat->name;
    if (input.format->iformat->long_name) info.containerDescription = input.format->iformat->long_name;
    info.durationSeconds = ReadDuration(input.format->duration, AVRational{1, AV_TIME_BASE});
    info.bitRate = ReadBitRate(input.format->bit_rate);
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        CheckProbeState(state);
        info.streams.push_back(ReadStreamInformation(input.format->streams[i]));
    }
    return info; // 字符串/数字已经复制；input析构关闭文件，报告仍然有效
}
// 窗口和CLI拿到探测结果后调用它，先排文件总信息，再遍历轨道，分别补充声音或画面的专有参数。
// KnownText、Duration和Rational等辅助函数统一处理单位与未知值；这里只生成UTF-8文字，不重新读取文件。
std::string FormatMediaInfo(const MediaInfo& info) {
    std::ostringstream text;
    text << "File (文件): " << info.path << '\n'
         << "Container (容器): " << KnownText(info.containerName) << " / " << KnownText(info.containerDescription) << '\n'
         << "Duration (总时长): " << Duration(info.durationSeconds) << '\n'
         << "Bit rate (总码率): " << Rate(info.bitRate) << '\n'
         << "Streams (轨道数量): " << info.streams.size() << '\n';
    for (const auto& stream : info.streams) {
        text << "\nStream #" << stream.index << " (轨道): " << stream.type << '\n'
             << "  Codec (编码): " << KnownText(stream.codecName) << " / " << KnownText(stream.codecDescription) << '\n'
             << "  Time base (时间刻度): " << Rational(stream.timeBase, false) << '\n'
             << "  Duration (轨道时长): " << Duration(stream.durationSeconds) << '\n'
             << "  Bit rate (轨道码率): " << Rate(stream.bitRate) << '\n'
             << "  Language (语言标记): " << KnownText(stream.language) << '\n';
        if (stream.type == "audio") {
            text << "  Sample rate (每秒采样数): " << Number(stream.sampleRate) << " Hz\n"
                 << "  Channels (声道数量): " << Number(stream.channels) << '\n'
                 << "  Channel layout (声道排列): " << KnownText(stream.channelLayout) << '\n'
                 << "  Sample format (样本存法): " << KnownText(stream.sampleFormat) << '\n';
        } else if (stream.type == "video") {
            text << "  Size (宽x高): " << Number(stream.width) << 'x' << Number(stream.height) << '\n'
                 << "  Pixel format (像素存法): " << KnownText(stream.pixelFormat) << '\n'
                 << "  Average frame rate (平均帧率): " << Rational(stream.averageFrameRate, true) << '\n'
                 << "  Reference frame rate (参考帧率): " << Rational(stream.referenceFrameRate, true) << '\n'
                 << "  Attached picture (是否封面): " << (stream.attachedPicture ? "是" : "否") << '\n';
        }
    }
    text << "\n说明：信息来自文件和FFmpeg探测；未知不是0。帧率字段不能单独证明视频恒定帧率。\n";
    return text.str();
}
}
