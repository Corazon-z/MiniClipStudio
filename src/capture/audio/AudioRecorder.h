#pragma once
// 录音会话输入与结果，不依赖Win32窗口；引用需覆盖整个worker调用。
#include "../DeviceCatalog.h"
#include <atomic>
#include <cstdint>
namespace miniclip {
struct CaptureOptions {
    // device是UTF-8设备标识；inputFile为空表示麦克风，非空表示文件转换实验。
    std::string device, inputFile; // inputFile 非空时使用同一链路转换文件
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
// 工作线程执行；调用期间 options 不变，cancel 与 progress 必须存活。
// 完整处理链路：输入包 → 解码帧 → 重采样 → PCM编码包 → WAV。
// const引用表示借用且不通过该引用修改；progress为可写引用。所有引用对象必须活到调用结束。
CaptureResult RunAudioCapture(const CaptureOptions&, const std::atomic<bool>& cancel,
    std::atomic<int64_t>& progress);
// 生成确定性测试声音；stereo默认false为单声道，true时左右频率不同以验证平面读取。
CaptureResult WriteSyntheticAudio(const std::wstring& outputFile, bool stereo = false);

}
