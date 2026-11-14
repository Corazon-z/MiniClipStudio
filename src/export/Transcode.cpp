// S03的转码总入口在这里：--export命令调用ExportMp4，把一个源素材转换为新的H.264/AAC MP4。
// libavformat负责打开文件和逐包读取，TranscodeTrack分别处理声音和画面的解码、过滤、编码，
// Mp4Output负责把成品包装进MP4、写完文件尾并发布。ExportMp4把这些步骤连起来，并汇总进度与指标。
// 整次会话由当前线程管理；轨道借用输入和输出，所以销毁时先清轨道，再释放它们依赖的文件会话。
#define NOMINMAX
#include "Transcode.h"
#include "TranscodeTiming.h"
#include "../media/encode/TranscodeTrack.h"
#include <filesystem>
#include <psapi.h>
#include <limits>

namespace miniclip {
namespace {
// ExportMp4在读包前后、进度回调返回后和发布前调用它；看到原子停止标志就抛取消异常，清理尚未完成的导出。
void CheckCancel(const std::atomic<bool>* cancel) {
    if (cancel && cancel->load()) throw TranscodeCancelled();
}
// libavformat读取时会查询这个回调是否中断；它只返回用户的停止意图，不抛异常或代替外层清理。
int InterruptExport(void* opaque) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(opaque);
    return cancel && cancel->load();
}
// ExportMp4在开始和结束附近调用GetProcessTimes，用两次累计值之差计算本次进程CPU时间。
// 这里合计所有线程的内核态和用户态时间，不等于用户等了多久，也不能单独说明哪段函数最耗CPU。
double ProcessCpuSeconds() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        throw std::runtime_error("Read process CPU time failed");
    const uint64_t ticks = (uint64_t{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime)
        + (uint64_t{user.dwHighDateTime} << 32 | user.dwLowDateTime);
    return ticks / 10000000.0;
}
// ExportMp4开始时先检查任务单：本地输入是否存在，输出会否覆盖，片段时间、帧率和音频规格是否在范围内。
// 宽高须同时指定或同时留空；实际编码尺寸的偶数和面积限制，稍后再由TranscodeTrack构造时检查。
void ValidateOptions(const TranscodeOptions& options) {
    if (!std::filesystem::is_regular_file(options.inputFile)) throw std::runtime_error("Input is not a local regular file");
    if (options.outputFile.empty() || _wcsicmp(std::filesystem::path(options.outputFile).extension().c_str(), L".mp4") != 0)
        throw std::runtime_error("Export output must use .mp4 extension");
    if (std::filesystem::exists(options.outputFile)) throw std::runtime_error("Export output already exists / 不覆盖已有文件");
    if (!std::isfinite(options.startSeconds) || !std::isfinite(options.durationSeconds)
        || options.startSeconds < 0 || options.durationSeconds < 0
        || options.startSeconds + options.durationSeconds > 86400)
        throw std::runtime_error("Clip time must be finite and within 24 hours");
    if (options.framesPerSecond < 1 || options.framesPerSecond > 120
        || options.processingThreads < 1 || options.processingThreads > 32)
        throw std::runtime_error("FPS must be 1..120; codec/filter threads must be 1..32");
    if (options.width < 0 || options.height < 0 || ((options.width == 0) != (options.height == 0)))
        throw std::runtime_error("Specify both output width and height, or neither");
    if ((options.sampleRate != 44100 && options.sampleRate != 48000) || (options.channels != 1 && options.channels != 2))
        throw std::runtime_error("Audio output: 44100/48000 Hz and mono/stereo only");
}
}
// --export从这里进入S03：检查参数后用libavformat识别文件，选中视频与可选音轨，计算两轨共用的裁剪起点。
// 再创建Mp4Output和TranscodeTrack，写容器头，将read_frame读出的包按轨道交给PushPacket继续处理。
// 输入结束后逐轨Finish取完延迟数据，再写文件尾、统计指标并Publish；正式发布之前的失败都不算成功。
TranscodeMetrics ExportMp4(const TranscodeOptions& options, const std::atomic<bool>* cancel,
    const std::function<void(const TranscodeProgress&)>& onProgress) {
    const auto started = std::chrono::steady_clock::now();
    const double cpuStarted = ProcessCpuSeconds();
    CheckCancel(cancel);
    ValidateOptions(options);
    // 先限定file协议并设置取消回调，再调用open_input/find_stream_info识别轨道，选择首条非封面视频和可选首音轨。
    TranscodeMetrics result;
    InputResources input;
    input.format = avformat_alloc_context();
    if (!input.format) throw std::bad_alloc();
    input.format->interrupt_callback = {InterruptExport, const_cast<std::atomic<bool>*>(cancel)};
    Check(av_dict_set(&input.options, "protocol_whitelist", "file", 0), "Set export local-file protocol");
    int status = avformat_open_input(&input.format, Utf8(std::filesystem::absolute(options.inputFile).wstring()).c_str(), nullptr, &input.options);
    CheckCancel(cancel);
    Check(status, "Open export input");
    status = avformat_find_stream_info(input.format, nullptr);
    CheckCancel(cancel);
    Check(status, "Read export streams");
    const AVStream* video = nullptr;
    const AVStream* audio = nullptr;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        const AVStream* stream = input.format->streams[i];
        if (!video && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = stream;
        if (!audio && options.includeAudio && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = stream;
    }
    if (!video) throw std::runtime_error("MP4 export requires a non-cover video stream; use --convert for audio WAV");
    // 两轨必须共用同一时间原点：优先取容器起点，未提供时取选中轨道的最早起点，避免抹掉原有的音画偏移。
    int64_t origin = input.format->start_time;
    if (origin == AV_NOPTS_VALUE) {
        origin = 0;
        bool found = false;
        for (const AVStream* stream : {video, audio}) {
            if (stream && stream->start_time != AV_NOPTS_VALUE) {
                const int64_t start = av_rescale_q(stream->start_time, stream->time_base, AVRational{1, AV_TIME_BASE});
                if (!found || start < origin) origin = start;
                found = true;
            }
        }
    }
    const int64_t offset = static_cast<int64_t>(std::llround(options.startSeconds * AV_TIME_BASE));
    if (origin > (std::numeric_limits<int64_t>::max)() - offset)
        throw std::runtime_error("Source time origin is too large");
    const int64_t clipStart = origin + offset;
    // 先建MP4输出，再建借用它的逐轨处理器，最后WriteHeader；C++反向析构会先清轨道，后释放输出流。
    Mp4Output output(options.outputFile);
    auto videoTrack = std::make_unique<TranscodeTrack>(video, output, options, clipStart, result, cancel);
    std::unique_ptr<TranscodeTrack> audioTrack;
    if (audio) { audioTrack = std::make_unique<TranscodeTrack>(audio, output, options, clipStart, result, cancel); result.audioTracks = 1; }
    { ScopedSeconds time(result.muxSeconds); output.WriteHeader(); }
    Packet packet = NewPacket();
    int64_t lastReportedFrames = -1;
    auto lastReport = started;
    // 主循环用这个回调隔一段时间报告进度，收尾时强制报一次；调用方可在回调内请求取消，返回后立即检查。
    auto report = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (onProgress && (force || (result.videoFrames != lastReportedFrames
            && (lastReportedFrames < 0 || now - lastReport >= std::chrono::milliseconds(250))))) {
            onProgress({result.videoFrames, result.audioSamples, result.videoFrames / double(options.framesPerSecond)});
            lastReportedFrames = result.videoFrames;
            lastReport = now;
            CheckCancel(cancel); // 回调可同步提出取消，不能在此之后继续假成功
        }
    };
    // read_frame可能读到任意轨道的包，用stream_index找到对应PushPacket；其他轨道跳过，每轮解除包引用并报告进度。
    for (;;) {
        CheckCancel(cancel);
        status = av_read_frame(input.format, packet.get());
        CheckCancel(cancel);
        if (status == AVERROR_EOF) break;
        if (status == AVERROR(EAGAIN)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        Check(status, "Read export packet");
        if (packet->stream_index == videoTrack->SourceIndex()) videoTrack->PushPacket(packet.get());
        else if (audioTrack && packet->stream_index == audioTrack->SourceIndex()) audioTrack->PushPacket(packet.get());
        av_packet_unref(packet.get());
        report(false);
    }
    // 文件结束不等于编码完成：先检查输入I/O，再让两轨Finish取完各级缓存中的尾部，最后确认确实产出了内容。
    if (input.format->pb) Check(input.format->pb->error, "Export input IO");
    videoTrack->Finish();
    if (audioTrack) audioTrack->Finish();
    report(true);
    if (result.videoFrames == 0 || (audioTrack && result.audioSamples == 0))
        throw std::runtime_error("Selected clip has no output video/audio samples");
    CheckCancel(cancel);
    { ScopedSeconds time(result.muxSeconds); output.Finish(); }
    // output.Finish已写完文件尾并关闭临时文件；接着采集指标、检查取消，再Publish改成正式文件名。
    // totalSeconds统计到发布成功，不包含函数返回时随后发生的局部对象析构。
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)))
        throw std::runtime_error("Read process memory counters failed");
    result.peakWorkingSetBytes = memory.PeakWorkingSetSize;
    result.outputBytes = output.Bytes();
    result.cpuSeconds = ProcessCpuSeconds() - cpuStarted;
    CheckCancel(cancel);
    output.Publish();
    result.totalSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}
}
