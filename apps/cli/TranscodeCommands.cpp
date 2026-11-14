// 这个文件实现S03的--export命令。RunAudioCommands调用这里后，把控制台参数
// 整理成ExportMp4的要求，并显示导出进度和性能报告；真正的解码、编码、封装都留在src中。
#include "TranscodeCommands.h"
#include "../../src/export/Transcode.h"
#include <Windows.h>
#include <cstdio>
#include <cwchar>
#include <limits>

namespace miniclip {
namespace {
std::atomic<bool> consoleCancel{false}; // 静态存储期，系统控制台回调不借用局部对象
// Windows收到Ctrl+C/Break后调用这里，写下consoleCancel，供ExportMp4在处理过程中检查。
// 这里只发协作式取消请求，不在系统回调中等待或释放仍由导出函数使用的资源。
BOOL WINAPI CancelHandler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    consoleCancel.store(true);
    return TRUE; // 只发请求，不在系统回调中等待或释放媒体资源
}
class ConsoleCancellation {
public:
    // RunTranscodeCommand在调用ExportMp4前创建它，重置停止请求并接通本次Ctrl+C回调。
    ConsoleCancellation() {
        consoleCancel.store(false);
        if (!SetConsoleCtrlHandler(CancelHandler, TRUE)) throw std::runtime_error("Register Ctrl+C handler failed");
    }
    // 导出命令离开作用域时注销回调；注销失败只能记录诊断，析构不能再抛出异常。
    ~ConsoleCancellation() {
        if (!SetConsoleCtrlHandler(CancelHandler, FALSE)) std::fprintf(stderr, "Remove console handler failed\n");
    }
};
// RunTranscodeCommand用这里读取尺寸、帧率和线程等整数选项。
// std::stol必须消费完整文字且数值落在int范围内，不能把“30abc”悄悄当作30。
int Integer(const wchar_t* value) {
    size_t used = 0;
    long number = std::stol(value, &used);
    if (used != wcslen(value) || number < (std::numeric_limits<int>::min)() || number > (std::numeric_limits<int>::max)())
        throw std::runtime_error("Invalid integer export argument");
    return static_cast<int>(number);
}
// --start和--duration先通过这里把文字秒数变成double，并拒绝未读完的后缀。
// 数值是否有限、是否超出允许范围，再由ExportMp4统一验证，避免CLI与媒体接口各写一套规则。
double Seconds(const wchar_t* value) {
    size_t used = 0;
    double number = std::stod(value, &used);
    if (used != wcslen(value)) throw std::runtime_error("Invalid seconds export argument");
    return number; // finite/范围由媒体接口统一验证
}
}
// RunAudioCommands识别--export后调用这里，先将选项写进TranscodeOptions，再创建取消处理器。
// ExportMp4在当前线程同步完成S03导出，并借用进度回调；它成功返回后才打印METRICS报告。
// 这里不拥有FFmpeg编码资源，明确取消单独返回3，其余错误交给上层入口报告。
int RunTranscodeCommand(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::puts("Usage: --export INPUT OUTPUT.mp4 [--size W H] [--fps N] [--start S] [--duration S]\n"
            "       [--no-audio] [--sample-rate 44100|48000] [--channels 1|2] [--threads 1..32]");
        return 2;
    }
    TranscodeOptions options;
    options.inputFile = argv[2]; options.outputFile = argv[3];
    for (int i = 4; i < argc; ++i) {
        const std::wstring flag = argv[i];
        if (flag == L"--no-audio") options.includeAudio = false;
        else if (flag == L"--size" && i + 2 < argc) {
            options.width = Integer(argv[++i]); options.height = Integer(argv[++i]);
        } else if (i + 1 < argc) {
            const wchar_t* value = argv[++i];
            if (flag == L"--fps") options.framesPerSecond = Integer(value);
            else if (flag == L"--start") options.startSeconds = Seconds(value);
            else if (flag == L"--duration") options.durationSeconds = Seconds(value);
            else if (flag == L"--sample-rate") options.sampleRate = Integer(value);
            else if (flag == L"--channels") options.channels = Integer(value);
            else if (flag == L"--threads") options.processingThreads = Integer(value);
            else throw std::runtime_error("Unknown export option");
        } else throw std::runtime_error("Missing export option value");
    }
    ConsoleCancellation cancellation;
    try {
        // ExportMp4在导出过程中同步调用这个lambda，报告累计视频帧、音频样本和媒体时间。
        // 回调只写控制台，不创建额外线程，也不接管媒体资源。
        const auto metrics = ExportMp4(options, &consoleCancel, [](const TranscodeProgress& progress) {
            std::fprintf(stderr, "Export progress / 导出进度: frames=%lld, audio_samples=%lld, video_seconds=%.3f\n",
                static_cast<long long>(progress.videoFrames), static_cast<long long>(progress.audioSamples), progress.mediaSeconds);
        });
        // ExportMp4只有在输出发布成功后才返回这些指标；接着按固定格式打印，方便测试脚本读取。
        // 阶段API墙钟不等同于CPU采样热点，也不能仅凭这份报告认定已经使用硬件加速。
        std::puts("MP4 saved / 导出成功（H.264 + 可选AAC）；未做旋转/HDR处理，输出为固定帧率。\nMETRICS_BEGIN");
        std::printf("{\"total_seconds\":%.6f,\"cpu_seconds\":%.6f,\"decode_seconds\":%.6f,\"filter_seconds\":%.6f,"
            "\"encode_seconds\":%.6f,\"mux_seconds\":%.6f,\"peak_working_set_bytes\":%llu,\"output_bytes\":%llu,"
            "\"video_frames\":%lld,\"decoded_video_frames\":%lld,\"audio_samples\":%lld,\"audio_tracks\":%d,"
            "\"width\":%d,\"height\":%d,\"fps\":%d,\"threads\":%d}\nMETRICS_END\n",
            metrics.totalSeconds, metrics.cpuSeconds, metrics.decodeSeconds, metrics.filterSeconds, metrics.encodeSeconds,
            metrics.muxSeconds, static_cast<unsigned long long>(metrics.peakWorkingSetBytes),
            static_cast<unsigned long long>(metrics.outputBytes), static_cast<long long>(metrics.videoFrames),
            static_cast<long long>(metrics.decodedVideoFrames), static_cast<long long>(metrics.audioSamples),
            metrics.audioTracks, metrics.width, metrics.height, options.framesPerSecond, options.processingThreads);
        return 0;
    } catch (const TranscodeCancelled& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 3; // 明确区分取消和成功/失败
    }
}
}
