// S05窗口把已经解码的BGRA画面交给这里绘制；媒体线程不接触窗口的HDC。
// 同一个绘制函数也能画到测试用内存位图，检查真实像素、方向和黑边，不只检查调用次数。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "../../../playback/PreviewTypes.h"

namespace miniclip {
// 窗口WM_PAINT和离屏测试共用这里：先清黑，再把自上向下的BGRA按原比例缩放到bounds中央。
// picture为空只清屏；尺寸/缓冲无效或GDI调用失败返回false，调用者决定怎样报告绘制错误。
bool DrawPreviewPicture(HDC target, const RECT& bounds, const PreviewPicture* picture);
// 只擦视频区域之外的面板背景，保护当前已显示画面；调用后恢复原DC裁剪状态。
bool ClearAroundPreview(HDC target, const RECT& client, const RECT& viewport, HBRUSH background);
// 窗口独占一份可复用的内存画布：黑边和图像先画完，再一次BitBlt呈现，避免中间清屏闪白/闪黑。
// 仅GUI线程使用，大小不变时复用DC/位图；不可复制GDI资源所有权。
class BufferedVideoSurface {
public:
    BufferedVideoSurface() = default;
    ~BufferedVideoSurface();
    BufferedVideoSurface(const BufferedVideoSurface&) = delete;
    BufferedVideoSurface& operator=(const BufferedVideoSurface&) = delete;
    // 只修改bounds区域；无面积不分配，非法源不覆盖当前窗口，失败返回false。
    bool Draw(HDC target, const RECT& bounds, const PreviewPicture* picture);
private:
    // 尺寸变化时重新建画布；先释放旧位图的选中关系，保证异常/失败不漏GDI对象。
    bool Resize(HDC target, int width, int height);
    void Release();
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    int width_ = 0, height_ = 0;
};
}
