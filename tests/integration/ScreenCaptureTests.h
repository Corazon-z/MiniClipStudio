// 区域坐标归一化和真实gdigrab采集测试；真实测试只抓取本测试自己的已知色块窗口。
#pragma once
#include <string>
namespace miniclip {
void TestScreenRegion();
void TestScreenCapture(const std::wstring& output);
}
