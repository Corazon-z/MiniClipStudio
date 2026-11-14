// S05的自动验证通过这个入口检查真正的GDI绘制像素，不需要可见窗口或输出音频设备。
#pragma once
// LifecycleTests调用后，测试画面方向、BGRA颜色、等比缩放黑边和无帧清屏；失败抛可读异常。
void TestVideoSurface();
