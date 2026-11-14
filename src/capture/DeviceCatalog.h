// 这个文件定义S00录音使用的麦克风目录。窗口的后台枚举任务和录音CLI调用
// ListAudioDevices获取设备标识/描述；它们是复制后的字符串，不再依赖FFmpeg清单。
// 这里只列输入设备，S04播放使用的默认扬声器不在这份清单里。
#pragma once
#include <string>
#include <vector>
namespace miniclip {
struct AudioDevice { std::string name, description; };
// 查询DirectShow输入源，按媒体类型挑出可录音设备，并复制其标识与显示名称。
// RecordingController或CLI得到返回值后即可使用自己的清单；查询失败抛异常，不会开始录音。
std::vector<AudioDevice> ListAudioDevices();
}
