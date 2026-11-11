#pragma once
// 同一个EXE的无GUI实验入口；解析参数，复用录制API，不调用ffmpeg CLI。
namespace miniclip { int RunAudioCommands(int argc, wchar_t** argv); }
