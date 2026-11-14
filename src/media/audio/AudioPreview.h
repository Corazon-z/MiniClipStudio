// S05的音频生产线程调用这里，输出带共同媒体时间和代次的48kHz双声道float PCM。
// S04的DecodeAudioFile保持原行为；这里额外负责跳转前置解码、样本级裁头和首音轨偏移。
#pragma once
#include "../../playback/PreviewTypes.h"
namespace miniclip {
PreviewDecodeResult DecodePreviewAudio(const PreviewSource& source, int64_t targetUs,
    PreviewReadControl control, const std::function<void(PreviewAudioBlock)>& consume);
}
