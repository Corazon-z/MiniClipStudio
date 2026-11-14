// 主窗口的“打开视频”与--preview命令共用S05预览窗口，不给旧录音窗口另写一套播放算法。
// Open返回后原消息循环继续工作，预览关闭前异步停止并回收VideoPlayer，不能用嵌套等待阻塞owner。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>

namespace miniclip {
// GUI空闲时调用；file为空先选本地文件，取消返回nullptr。打开后暂时禁用owner，关闭再恢复。
HWND OpenVideoPreviewWindow(HWND owner, const std::wstring& file = L"");
// --preview使用同一窗口过程，但自己拥有消息循环；file为空时也允许弹文件对话框。
int RunVideoPreviewWindow(const std::wstring& file);
// 主循环先交这里处理预览窗口的Tab等控件导航；返回true表示消息已被处理。
bool ProcessVideoPreviewMessage(MSG& message);
// 主窗口通过这两个接口判断互斥状态、请求预览先关闭；都只在同一个GUI控制线程调用。
bool IsVideoPreviewWindowOpen();
void RequestVideoPreviewClose();
}
