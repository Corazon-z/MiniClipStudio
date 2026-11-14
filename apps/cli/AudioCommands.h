// 这个文件声明同一个EXE的命令行入口。wmain收到命令参数后调用它，
// 再分别进入S01探测、S02首帧导出、S03转码、S04音频、S05预览，或S00录音与验证实验。
#pragma once
namespace miniclip {
// wmain把参数交给这里后，按命令选择相应的媒体函数，并把处理结果变成退出码。
// 这里组织参数和调用顺序；解码、编码、播放均复用src内的实现，不调用ffmpeg程序。
int RunAudioCommands(int argc, wchar_t** argv);
}
