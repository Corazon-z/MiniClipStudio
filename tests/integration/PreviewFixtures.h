// S05用这些已知画面/声音检查跨关键帧seek、B帧排空、VFR和首音轨偏移，不改S02/S03已有夹具。
// 输入固定为NUT：H.264视频带多个GOP，可选两路PCM音轨。精确时间与灰度由生成函数确定。
#pragma once
#include <string>

enum class PreviewFixtureKind { AudioVideo, DelayedAudio, SilentVideo, VariableRate, ShortAudio };

// 生成4秒、100帧的64×48素材。帧i的Y=32+2*i、UV=128；普通PTS间隔40ms，VFR交替20/60ms。
// 所有轨道先加0.5秒绝对起点；DelayedAudio额外把首音轨推后0.2秒，第二音轨不推后。
// 第一音轨内容先440Hz、2秒后880Hz；第二音轨1760Hz。目标已存在就失败，不覆盖旧文件。
// ShortAudio只写前2秒的两条音轨，视频仍是4秒，用来检查音频先结束后是否补静音并继续显示画面。
void MakePreviewFixture(const std::wstring& file, PreviewFixtureKind kind);
