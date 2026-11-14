// 这个文件实现S00麦克风枚举。RecordingController替窗口在后台调用它，
// CLI的--devices/--record等入口也使用同一份实现；这里只查设备，不打开录音会话。
#include "DeviceCatalog.h"
#include "../media/ffmpeg/FfmpegSupport.h"
namespace miniclip {
// 先找到FFmpeg的dshow输入后端，再用avdevice_list_input_sources获取设备清单。
// 通过media_types辨认音频输入、复制所需文字后释放原清单；成功和异常路径都要归还它。
std::vector<AudioDevice> ListAudioDevices() {
    // 注册设备模块后查找dshow；找到后才能向该后端询问有哪些输入源。
    avdevice_register_all();
    const AVInputFormat* format = av_find_input_format("dshow");
    if (!format) throw std::runtime_error("DirectShow input unavailable");
    AVDeviceInfoList* list = nullptr;
    // list由FFmpeg分配，本函数负责在正常和异常路径调用free_list_devices。
    int ret = avdevice_list_input_sources(format, nullptr, nullptr, &list);
    std::vector<AudioDevice> devices;
    try {
        Check(ret, "Enumerate DirectShow devices");
        for (int i = 0; list && i < list->nb_devices; ++i) {
            const AVDeviceInfo* info = list->devices[i];
            bool audio = false;
            // 设备可能支持多种媒体类型，不能只凭名称猜是否为麦克风。
            for (int j = 0; j < info->nb_media_types; ++j)
                if (info->media_types[j] == AVMEDIA_TYPE_AUDIO) audio = true;
            if (audio && info->device_name) devices.push_back({info->device_name,
                info->device_description ? info->device_description : info->device_name});
        }
    // 清理后继续传播原异常，避免用空列表掩盖枚举失败。
    } catch (...) { avdevice_free_list_devices(&list); throw; }
    avdevice_free_list_devices(&list);
    return devices;
}
}
