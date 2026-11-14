# 命令行入口

路径：`apps/cli/`。整个仓库是产品，本目录是一个职责模块。

## 职责

参数解析、实验与自动验证，复用生产媒体链路。

## 状态与实施规则

2026-11-11建立目录。说明文件用于保留目录和解释边界，不代表功能完成。实际代码与迁移状态以docs/MODULES.md和工作记录为准。

先完成正确、可验证功能，再根据测量优化；每个新函数解释输入/输出、错误、所有权和线程。不得把占位类当成实现；不为每个目录机械增加DLL或线程。

## 已落地文件（2026-11-11）

- `AudioCommands.h`：无GUI命令入口声明。
- `AudioCommands.cpp`：参数解析、转换/录音/取消/合成实验，调用同一生产链路。

这里只实现列出的基础能力；其余功能继续按阶段验收，不宣称完整商业质量。

2026-11-13：S03基础CLI已落地，实际文件/所有权/调用顺序见docs/TRANSCODING.md与docs/MODULES.md；验证和性能边界见worklogs/2026-11-13-s03-transcode.md、PERFORMANCE_WORKFLOW.md。目录中其他占位说明不代表实现。

## S04音频播放

PlaybackCommands.h/.cpp实现`--play INPUT [--volume 0..100]`。P暂停/继续，Q/Ctrl+C停止；调用同一AudioPlayer，不调用ffmpeg命令行。0完成/3停止/1失败/2用法错误。详见docs/AUDIO_PLAYBACK.md。

## S06已实现范围（2026-11-14）

新增--camera/--cameras/--camera-modes/--camera-options/--camera-record；--camera-simulate明确是测试输入。 具体使用、文件分工及验收边界见[摄像头说明](../../docs/CAMERA_CAPTURE.md)。

## 本次区域与重绘更新（2026-11-14）

新增--screen-record --select NEW.mp4或X Y W H NEW.mp4；区域无声，Ctrl+C停止保存。 使用和边界见[区域说明](../../docs/SCREEN_RECORDING.md)。
