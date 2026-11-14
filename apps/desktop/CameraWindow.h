// S06摄像头窗口：设备/实际模式、只预览、录制、倒计时、停止保存；后台任务由CameraSession拥有。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
namespace miniclip {
// 主窗口非阻塞打开；owner暂时禁用，关窗等待采集真实退出后恢复。
HWND OpenCameraWindow(HWND owner);
// --camera命令使用同一窗口，单独运行消息循环。
int RunCameraWindow();
bool IsCameraWindowOpen();
void RequestCameraClose();
bool ProcessCameraMessage(MSG& message);
}
