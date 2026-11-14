// 这个文件把生产用DrawPreviewPicture画到真实CreateDIBSection内存位图，再读回像素。
// 它验证GDI路径而非仿造绘制算法；可见窗口布局与人的观感仍由单独的GUI验收完成。
#include "VideoSurfaceTests.h"
#include "../../src/platform/windows/output/VideoSurface.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#pragma comment(lib, "Gdi32.lib")
using namespace miniclip;
namespace {
// 每个像素断言都通过这里报告明确原因，由测试入口捕获异常并给出失败退出码。
void RequireSurface(bool good, const char* reason) {
    if (!good) throw std::runtime_error(reason);
}

// 测试拥有一张自上向下的32位DIB，并把它选入内存DC；离开作用域时恢复原对象后释放。
struct SurfaceCanvas {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ original = nullptr;
    uint32_t* pixels = nullptr;
    int width, height;
    // 建立内存DC和DIB并记住原选中对象；构造中途失败也要执行Release清理已有资源。
    SurfaceCanvas(int w, int h) : width(w), height(h) {
        try {
            dc = CreateCompatibleDC(nullptr);
            if (!dc) throw std::runtime_error("Create surface test DC failed");
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            void* memory = nullptr;
            bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &memory, nullptr, 0);
            if (!bitmap || !memory) throw std::runtime_error("Create surface test DIB failed");
            pixels = static_cast<uint32_t*>(memory);
            original = SelectObject(dc, bitmap);
            if (!original || original == HGDI_ERROR) throw std::runtime_error("Select surface test bitmap failed");
            std::fill_n(pixels, width * height, 0x00555555U);
        } catch (...) { Release(); throw; }
    }
    // 这组GDI句柄只有一个所有者，不能复制后重复归还。
    SurfaceCanvas(const SurfaceCanvas&) = delete;
    SurfaceCanvas& operator=(const SurfaceCanvas&) = delete;
    // 测试作用域结束后恢复DC原对象，再释放本测试的位图和DC。
    ~SurfaceCanvas() { Release(); }
    // 构造失败也走同一清理路径，避免测试报错时泄漏GDI资源。
    void Release() {
        if (dc && original && original != HGDI_ERROR) SelectObject(dc, original);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        bitmap = nullptr; dc = nullptr; original = nullptr;
    }
    // 读取已flush位图的RGB值，忽略BI_RGB未参与颜色计算的最高字节。
    uint32_t At(int x, int y) const { return pixels[y * width + x] & 0x00FFFFFFU; }
    // 给生产绘制函数同一张测试画布的完整目标区域。
    RECT Bounds() const { return {0, 0, width, height}; }
};

// 测试像素写入BGRA数组；A不参与BI_RGB颜色绘制，检查时只比较RGB三个字节。
PreviewPicture Picture(int width, int height, const std::vector<uint32_t>& colors) {
    RequireSurface(colors.size() == static_cast<size_t>(width) * height, "Invalid test color count");
    PreviewPicture picture;
    picture.width = width; picture.height = height; picture.bgra.resize(colors.size() * 4);
    std::memcpy(picture.bgra.data(), colors.data(), picture.bgra.size());
    return picture;
}
// GDI可能批量执行绘制；CPU读DIB像素前先等待这些写入提交完成。
void FlushSurface() { RequireSurface(GdiFlush() != FALSE, "Flush surface test GDI writes failed"); }
}

// LifecycleTests或独立验证入口调用这里，按已知颜色实际绘图并检查像素、方向、比例和清屏。
void TestVideoSurface() {
    constexpr uint32_t red = 0x00FF0000, green = 0x0000FF00, blue = 0x000000FF, white = 0x00FFFFFF;
    SurfaceCanvas canvas(8, 8);
    auto picture = Picture(2, 2, {red, green, blue, white});
    const auto original = picture.bgra;
    RequireSurface(DrawPreviewPicture(canvas.dc, canvas.Bounds(), &picture), "Draw BGRA test picture failed");
    FlushSurface();
    RequireSurface(canvas.At(0, 0) == red && canvas.At(7, 0) == green &&
        canvas.At(0, 7) == blue && canvas.At(7, 7) == white, "BGRA channel order or top-down orientation is wrong");
    RequireSurface(picture.bgra == original, "Surface renderer modified source pixels");

    // 横画面放进正方形：有效区域在中间四行，上下各两行必须是黑边。
    picture = Picture(4, 2, {red, red, red, red, blue, blue, blue, blue});
    RequireSurface(DrawPreviewPicture(canvas.dc, canvas.Bounds(), &picture), "Draw landscape letterbox failed");
    FlushSurface();
    RequireSurface(canvas.At(4, 0) == 0 && canvas.At(4, 1) == 0 && canvas.At(4, 6) == 0 && canvas.At(4, 7) == 0,
        "Landscape picture lost top/bottom black bars");
    RequireSurface(canvas.At(4, 2) == red && canvas.At(4, 5) == blue, "Landscape aspect fit changed picture orientation");

    picture = Picture(2, 4, {green, green, green, green, white, white, white, white});
    RequireSurface(DrawPreviewPicture(canvas.dc, canvas.Bounds(), &picture), "Draw portrait letterbox failed");
    FlushSurface();
    RequireSurface(canvas.At(0, 4) == 0 && canvas.At(1, 4) == 0 && canvas.At(6, 4) == 0 && canvas.At(7, 4) == 0,
        "Portrait picture lost left/right black bars");
    RequireSurface(canvas.At(2, 0) == green && canvas.At(5, 7) == white, "Portrait aspect fit changed picture orientation");

    RequireSurface(DrawPreviewPicture(canvas.dc, canvas.Bounds(), nullptr), "Clear empty preview failed");
    FlushSurface();
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        RequireSurface(canvas.At(x, y) == 0, "Empty preview did not clear old picture");
    picture.bgra.resize(4);
    RequireSurface(!DrawPreviewPicture(canvas.dc, canvas.Bounds(), &picture), "Undersized picture buffer was accepted");
    RequireSurface(!DrawPreviewPicture(nullptr, canvas.Bounds(), nullptr), "Null target DC was accepted");
    RequireSurface(DrawPreviewPicture(canvas.dc, RECT{0, 0, 0, 0}, nullptr), "Empty viewport should be a successful no-op");
    FlushSurface();

    // 绘制只影响调用者给定的矩形，不能擦掉旁边的控件或其他面板。
    std::fill_n(canvas.pixels, 64, 0x00555555U);
    RequireSurface(DrawPreviewPicture(canvas.dc, RECT{2, 2, 6, 6}, nullptr), "Clear inset preview failed");
    FlushSurface();
    RequireSurface(canvas.At(0, 0) == 0x00555555 && canvas.At(7, 7) == 0x00555555 && canvas.At(3, 3) == 0,
        "Surface renderer painted outside supplied bounds");
    // 双缓冲必须保留相同颜色/黑边；非法源不能把窗口上的上一幅图清掉。
    BufferedVideoSurface buffered;
    picture = Picture(2, 2, {red, green, blue, white});
    RequireSurface(buffered.Draw(canvas.dc, canvas.Bounds(), &picture), "Buffered draw failed");
    FlushSurface();
    RequireSurface(canvas.At(0, 0) == red && canvas.At(7, 7) == white, "Buffered pixel orientation changed");
    auto invalid = picture; invalid.bgra.clear();
    RequireSurface(!buffered.Draw(canvas.dc, canvas.Bounds(), &invalid), "Buffered invalid picture accepted");
    FlushSurface();
    RequireSurface(canvas.At(0, 0) == red, "Failed draw exposed a cleared intermediate frame");
    for (int i = 0; i < 100; ++i) {
        const RECT bounds = (i % 2) ? RECT{2, 2, 6, 6} : canvas.Bounds();
        RequireSurface(buffered.Draw(canvas.dc, bounds, &picture), "Buffered resize/reuse failed");
    }
    RequireSurface(buffered.Draw(canvas.dc, RECT{}, nullptr), "Buffered zero size failed");
    const RECT inset{2,2,6,6};
    RequireSurface(ClearAroundPreview(canvas.dc, canvas.Bounds(), inset, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH))), "Clear panel surround failed");
    FlushSurface();
    RequireSurface(canvas.At(0,0) == white && canvas.At(3,3) != white, "Panel background erased the video viewport");
    std::puts("PASS: GDI BGRA pixels, top-down orientation, aspect-fit black bars, clear and input bounds");
}
