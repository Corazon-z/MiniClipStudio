// S06在没有摄像头时仍可验证预览像素、停止语义和后台会话寿命；不替代真实设备测试。
#pragma once
#include <string>
namespace miniclip {
// 输入是已知64×48/4秒测试夹具；输出路径必须不存在，失败抛异常使测试进程非零退出。
void TestCameraSession(const std::wstring& source, const std::wstring& output);
// 必须由测试脚本放在独立控制台中运行，防止测试信号影响用户进程。
// 在真实编码过程中触发Ctrl+C/Break，再走同一原子请求与排空发布链路。
void TestConsoleRecordingStop(const std::wstring& source, const std::wstring& output, bool controlBreak);
}
