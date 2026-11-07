#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
struct AudioDevice { std::string name, description; };
struct CaptureOptions {
    std::string device, inputFile; // inputFile 非空时使用同一链路转换文件
    std::wstring outputFile;
    double durationSeconds = 0;
};
struct CaptureResult {
    bool success = false, cancelled = false;
    int64_t samples = 0; // 每声道样本数
    std::string message;
};
std::vector<AudioDevice> ListAudioDevices();
// 工作线程执行；调用期间 options 不变，cancel 与 progress 必须存活。
CaptureResult RunAudioCapture(const CaptureOptions&, const std::atomic<bool>& cancel,
    std::atomic<int64_t>& progress);
CaptureResult WriteSyntheticAudio(const std::wstring& outputFile, bool stereo = false);
