// 这个文件声明S04音频播放的命令行入口。RunAudioCommands识别--play后转到这里，
// 让命令行和窗口共用AudioPlayer，只把输入方式换成参数、P/Q键和Ctrl+C。
#pragma once
namespace miniclip {
// 读取文件路径和音量后启动AudioPlayer；播放期间接收暂停、继续和停止请求。
// 必须等播放线程真正退出才返回，并用不同退出码表示自然完成、主动停止和失败。
int RunPlaybackCommand(int argc, wchar_t** argv);
}
