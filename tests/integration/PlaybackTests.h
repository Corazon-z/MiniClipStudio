// 这个测试模块验证S04的声音转换、暂停继续、停止和设备回收。
// DecodePlaybackFixture把解码样本交给数值测试，TestPlaybackDevice让真实WASAPI设备静音消费数据。
#pragma once
#include <string>
// 调用S04的DecodeAudioFile，将实际得到的float48k双声道样本写到测试文件供Python检查。
// 同时检查每块不超过1024帧，以及预先取消时不会先访问输入路径。
void DecodePlaybackFixture(const std::wstring& input, const std::wstring& output);
// 使用真实默认WASAPI设备静音播放，检查S04暂停继续、尾部消费、满队列停止、重启和错误收尾。
// 输入应至少2秒，留出验证暂停和恢复的时间；这个检查不代替音质试听。
void TestPlaybackDevice(const std::wstring& input);
