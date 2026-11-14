// S05测试通过真实解码结果、硬件时钟和代次检查验证播放，不把解码计数当同步或可见窗口验收。
#pragma once
#include <string>
// 写出解码PTS/灰度和PCM数据，供Python按已知素材独立核对；输出前缀必须是新路径。
void InspectPreviewDecoders(const std::wstring& input, double targetSeconds, const std::wstring& outputPrefix);
// 真实默认设备静音检查暂停seek、快速替换、自然结束/EOF后seek、队列和失败回收。
void TestVideoPlayback(const std::wstring& input);
// 完整播放并记录音频时钟和邮箱画面区间差；这些观测不冒充物理屏幕/听觉端到端延迟。
void MeasureVideoPlayback(const std::wstring& input);
