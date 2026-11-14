// 这个测试模块为S02视频解码和S03转码准备一段已知8帧的视频。
// 测试程序调用MakeDelayedVideoFixture生成素材，再检查解码器是否取出了最后的延迟画面。
#pragma once
#include <string>
// S02/S03测试通过这个函数生成8帧MPEG-4视频。画面有B帧延迟，适合检查EOF后是否取尽结果。
// 路径扩展名决定使用AVI还是NUT；目标已经存在时直接报错，保留原文件。
void MakeDelayedVideoFixture(const std::wstring& path);
