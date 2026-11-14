// 这个文件声明S03转码导出的命令行入口。RunAudioCommands识别--export后调用它，
// 将尺寸、帧率、裁剪等参数交给ExportMp4，并在控制台显示进度与测量结果。
#pragma once
namespace miniclip {
// 把命令选项整理成TranscodeOptions，注册Ctrl+C取消，再同步调用ExportMp4。
// 导出完成后打印报告；明确取消返回3，编码封装的具体实现不依赖这个控制台入口。
int RunTranscodeCommand(int argc, wchar_t** argv);
}
