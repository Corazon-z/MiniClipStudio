// 这个测试模块检查S03是否真的生成了内容和时间都正确的MP4文件。
// MakeTranscodeFixture准备标准输入，InspectTranscode重开输出核对，TestTranscodeCancellation检查停止后没有假成品。
#pragma once
#include <string>
// 为S03生成一条原始视频和两条PCM音轨，声音频率、画面亮度和时间都预先确定。
// audioOffset用来测试音轨延迟，benchmark选择更大的重复计时素材。
void MakeTranscodeFixture(const std::wstring& path, bool audioOffset = false, bool benchmark = false);
// S03完成导出后重新打开成品，检查每条轨道的DTS、包末尾时间以及真正解码出来的画面。
// 结果打印为JSON，供Python核对帧数和首尾亮度，避免只相信导出器自己的报告。
void InspectTranscode(const std::wstring& path);
// 分别在导出开始前和已产出画面后请求取消，检查ExportMp4是否报告专用取消异常。
// 两次取消都必须保持目标文件不存在，防止把半成品当成导出成功。
void TestTranscodeCancellation(const std::wstring& input, const std::wstring& output);
