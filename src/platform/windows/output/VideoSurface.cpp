// 这个文件实现S05的软件画面呈现：VideoPreviewWindow在GUI线程的WM_PAINT里调用它，
// 将PreviewPicture的自有BGRA数据交给StretchDIBits，保持比例并把剩余区域填成黑色。
#include "VideoSurface.h"
#include <algorithm>
#include <limits>

namespace miniclip {
bool ClearAroundPreview(HDC target, const RECT& client, const RECT& viewport, HBRUSH background) {
    if (!target || !background) return false;
    const int saved = SaveDC(target);
    if (!saved) return false;
    const int clipped = ExcludeClipRect(target, viewport.left, viewport.top, viewport.right, viewport.bottom);
    const bool filled = clipped != ERROR && FillRect(target, &client, background) != 0;
    const bool restored = RestoreDC(target, saved) != FALSE;
    return filled && restored;
}
BufferedVideoSurface::~BufferedVideoSurface() { Release(); }
// 位图仍被选入DC时不能直接删除；先恢复DC原对象，再归还本模块的资源。
void BufferedVideoSurface::Release() {
    if (memory_ && original_ && original_ != HGDI_ERROR) SelectObject(memory_, original_);
    if (bitmap_) DeleteObject(bitmap_);
    if (memory_) DeleteDC(memory_);
    memory_ = nullptr; bitmap_ = nullptr; original_ = nullptr; width_ = height_ = 0;
}
bool BufferedVideoSurface::Resize(HDC target, int width, int height) {
    if (memory_ && width == width_ && height == height_) return true;
    Release();
    memory_ = CreateCompatibleDC(target);
    if (!memory_) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bitmap_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap_ || !bits) { Release(); return false; }
    original_ = SelectObject(memory_, bitmap_);
    if (!original_ || original_ == HGDI_ERROR) { Release(); return false; }
    width_ = width; height_ = height; return true;
}
// 失败的清屏/像素校验只发生在后台画布，成功后才把完整内容交给窗口。
bool BufferedVideoSurface::Draw(HDC target, const RECT& bounds, const PreviewPicture* picture) {
    if (!target) return false;
    const int64_t width = int64_t{bounds.right} - bounds.left, height = int64_t{bounds.bottom} - bounds.top;
    if (width <= 0 || height <= 0) return true;
    if (width > 16384 || height > 16384 || width * height > 64LL * 1024 * 1024) return false;
    if (!Resize(target, static_cast<int>(width), static_cast<int>(height))) return false;
    if (!DrawPreviewPicture(memory_, RECT{0, 0, width_, height_}, picture)) return false;
    return BitBlt(target, bounds.left, bounds.top, width_, height_, memory_, 0, 0, SRCCOPY) != FALSE;
}
// 预览窗WM_PAINT和离屏测试都从这里清屏并绘图；先验证自有BGRA数据，再用top-down DIB居中等比缩放。
bool DrawPreviewPicture(HDC target, const RECT& bounds, const PreviewPicture* picture) {
    if (!target) return false;
    const int64_t width = static_cast<int64_t>(bounds.right) - bounds.left;
    const int64_t height = static_cast<int64_t>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0) return true; // 最小化或布局暂时没有面积时无需画图。
    if (width > (std::numeric_limits<int>::max)() || height > (std::numeric_limits<int>::max)()) return false;
    if (!FillRect(target, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)))) return false;
    if (!picture) return true;
    if (picture->width <= 0 || picture->height <= 0 || picture->width > 16384 || picture->height > 16384 ||
        static_cast<uint64_t>(picture->width) * picture->height > 64ULL * 1024 * 1024 ||
        static_cast<uint64_t>(picture->width) * picture->height * 4 != picture->bgra.size()) return false;

    // 按较紧的一边计算缩放尺寸，使用64位中间值避免乘法溢出，不把画面拉扁。
    int64_t fitWidth = width;
    int64_t fitHeight = static_cast<int64_t>(picture->height) * width / picture->width;
    if (fitHeight > height) {
        fitHeight = height;
        fitWidth = static_cast<int64_t>(picture->width) * height / picture->height;
    }
    const int drawWidth = static_cast<int>(std::max<int64_t>(1, fitWidth));
    const int drawHeight = static_cast<int>(std::max<int64_t>(1, fitHeight));
    const int x = bounds.left + (static_cast<int>(width) - drawWidth) / 2;
    const int y = bounds.top + (static_cast<int>(height) - drawHeight) / 2;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = picture->width;
    info.bmiHeader.biHeight = -picture->height; // 负高度告诉GDI第一行在上面，不用倒转像素数组。
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const int previous = SetStretchBltMode(target, COLORONCOLOR);
    if (!previous) return false;
    const int copied = StretchDIBits(target, x, y, drawWidth, drawHeight, 0, 0,
        picture->width, picture->height, picture->bgra.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    const bool restored = SetStretchBltMode(target, previous) != 0;
    return copied > 0 && restored;
}
}
