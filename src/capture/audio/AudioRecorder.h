// 这个文件定义S00录音及音频转WAV实验的参数、结果和处理入口。
// 窗口通过RecordingController在后台调用RunAudioCapture，CLI也可直接使用同一链路。
// 它只负责声音到WAV，不是S03的H.264/AAC视频转码接口，也不负责S04设备播放。
#pragma once
#include "../DeviceCatalog.h"
#include <atomic>
#include <cstdint>
namespace miniclip {
struct CaptureOptions {
    // device是UTF-8设备标识；inputFile为空表示麦克风，非空表示文件转换实验。
    std::string device, inputFile;
    // Windows宽字符输出路径，支持中文；交给FFmpeg前显式转成UTF-8。
    std::wstring outputFile;
    // 单位为秒；0表示不自动计时停止，等用户发出取消请求。
    double durationSeconds = 0;
};
// 一次任务的结果。GUI必须在线程退出并join后再读取这些普通成员。
struct CaptureResult {
    // success只在WAV正常收尾并发布后置true；cancelled用于尚无有效录音的取消。
    bool success = false, cancelled = false;
    int64_t samples = 0; // 每声道样本数
    // 成功路径或可读错误说明；不保存FFmpeg指针，避免结果带出失效资源。
    std::string message;
};
// RecordingController或CLI调用这里后，依次打开输入、解码音轨、交给WavWriter转换并保存。
// 该函数一直在调用者线程中运行，所以窗口不能直接调用它阻塞消息循环。
// options在处理期间不变，cancel和progress借用到返回为止，用atomic传递停止请求与进度。
CaptureResult RunAudioCapture(const CaptureOptions&, const std::atomic<bool>& cancel,
    std::atomic<int64_t>& progress);
// --synthetic命令和自动验证调用它，生成1秒44.1kHz的FLTP样本，再走真实WavWriter链路。
// FLTP把各声道分开存；stereo=true时左右分别440/880Hz，方便发现漏读或交换声道的问题。
CaptureResult WriteSyntheticAudio(const std::wstring& outputFile, bool stereo = false);

}
