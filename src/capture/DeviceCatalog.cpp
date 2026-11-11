// 复制设备元数据后释放FFmpeg清单；不打开录音、不操作窗口。
#include "DeviceCatalog.h"
#include "../media/ffmpeg/FfmpegSupport.h"
namespace miniclip {
std::vector<AudioDevice> ListAudioDevices() {
    avdevice_register_all();
    const AVInputFormat* format = av_find_input_format("dshow");
    if (!format) throw std::runtime_error("DirectShow input unavailable");
    AVDeviceInfoList* list = nullptr;
    // 返回设备列表归调用者释放；成功与异常路径都必须free_list_devices。
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
    // 捕获任何异常以释放清单，再用throw重新抛原异常，不吞掉错误。
    } catch (...) { avdevice_free_list_devices(&list); throw; }
    avdevice_free_list_devices(&list);
    return devices;
}
}
