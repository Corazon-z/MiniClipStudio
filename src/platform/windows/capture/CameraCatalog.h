// S06用这个模块查询摄像头及驱动实际公布的采集模式；它不录制、不创建窗口。
// 设备和模式查询使用DirectShow/COM，实际采集使用FFmpeg；返回值复制为普通C++数据。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace miniclip {
struct CameraDevice { std::string name, description; };
struct CameraMode {
    int width = 0, height = 0;
    int64_t frameInterval = 0; // DirectShow单位100ns，保留分数帧率而不擅自取整。
    std::string codec, pixelFormat, description;
    bool supported = false;
};
// 只列出视频输入，name保留与FFmpeg dshow一致的唯一设备名，description供人阅读。
std::vector<CameraDevice> ListCameraDevices();
// 打开所选摄像头的输出引脚能力；每次调用在当前线程建立并归还COM对象。
std::vector<CameraMode> ListCameraModes(const std::string& deviceName);
}
