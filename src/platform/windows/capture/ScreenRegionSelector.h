// 区域录屏用物理桌面像素描述范围；Win32选择器只收集矩形，不采集/编码媒体。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <optional>
namespace miniclip {
struct ScreenRegion { int left = 0, top = 0, width = 0, height = 0; };
// 将任意拖动方向规范化、裁到桌面内，并将宽高向内收为偶数，满足当前H.264/YUV420要求。
// 小于16像素或超过4096边长/16M像素时拒绝；不会静默选择整个屏幕。
std::optional<ScreenRegion> NormalizeScreenRegion(POINT start, POINT end, const RECT& desktop);
// 返回物理坐标下的虚拟桌面范围，支持位于主屏左/上方的负坐标。
RECT PhysicalDesktopBounds();
// 暂时禁用owner，拖动左键完成选择；Esc/右键取消。只在GUI线程、无采集任务时调用。
std::optional<ScreenRegion> SelectScreenRegion(HWND owner);
}
