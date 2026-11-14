// S05的视频生产线程调用这里，把选中视频轨解成拥有独立BGRA数据和源PTS的画面。
// 它保留可变帧率，seek后选覆盖目标时刻的画面；不编码、不操作窗口或播放设备。
#pragma once
#include "../../playback/PreviewTypes.h"
namespace miniclip {
PreviewDecodeResult DecodePreviewVideo(const PreviewSource& source, int64_t targetUs,
    PreviewReadControl control, const std::function<void(PreviewPicture)>& consume);
}
