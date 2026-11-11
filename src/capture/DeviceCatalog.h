#pragma once
// 设备目录：返回自有UTF-8字符串，允许后台枚举；失败抛可读异常。
#include <string>
#include <vector>
namespace miniclip {
struct AudioDevice { std::string name, description; };
std::vector<AudioDevice> ListAudioDevices();
}
