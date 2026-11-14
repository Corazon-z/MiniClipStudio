# 顺着功能读源码

S06当前入口（2026-11-14）：基础摄像头预览/可选麦克风MP4已实现，见[摄像头说明](CAMERA_CAPTURE.md)。本篇原有功能仍按自己的范围使用；S06复用不表示它新增所有摄像头控制、HDR或设备恢复。当前学习优先读本地六册，历史段落按日期理解。

更新：2026-11-14。先从[S01—S04功能串讲](FEATURES_S01_S04_WALKTHROUGH.md)选择一个功能，再沿真实函数名打开源码。本项目的65个自有头/源文件已改成连贯的中文说明，不再把同一意思拆成“专业／白话”两套标签。

## 文件和函数会怎样介绍自己

文件开头先说明它在某个功能里承担什么，再说明谁会调用它、会借助哪些库完成工作。例如MediaProbe模块负责S01：窗口后台或CLI把路径交给ProbeMediaFile，后者用FFmpeg读取参数，FormatMediaInfo再把参数排成文字。

函数前也沿着这件事继续解释。例如：

```cpp
// S01的窗口后台任务或--probe命令通过这个函数读取文件信息。
// 它打开并探测文件，再把容器与各轨参数复制到MediaInfo；返回时关闭输入，调用方仍可显示结果。
MediaInfo ProbeMediaFile(const std::wstring& path, const std::atomic<bool>* cancel = nullptr);
```

长函数内部再接着说“先打开输入”“这个循环只发送所选轨道的包”“结束后还需排空”等步骤，并写出所用函数名。短工具只说明它在这条处理链中解决什么问题，避免每行都重复“定义变量、赋值、返回”。

资源和线程说明也放在相关位置：谁创建谁释放，哪些地址只是借用，等待什么条件，停止后由谁join。注释帮助理解代码的意图，实际顺序与错误分支仍需对照下面的实现。

## 这些库在哪些功能里使用

这里说的FFmpeg库是现成工具；MediaProbe、TranscodeTrack和AudioPlayer等则是我们组织工具完成任务的本地模块。一个.h/.cpp不必各自生成一个独立库。

| 库或接口 | 在当前功能里怎样使用 |
|---|---|
| libavformat | S01打开文件、读轨道参数；S02—S04读取编码包；WAV和S03 MP4由它组织文件结构 |
| libavcodec | 解码声音/画面；S02编码PNG，S03编码H.264/AAC，原录音使用PCM编码器 |
| libavutil | 共同使用帧、声道布局、格式名称、时间换算和错误工具 |
| libavdevice | 原S00录音用它接入DirectShow麦克风，供设备枚举和采集 |
| libswscale | S02的ConvertToRgb使用它转换像素；S03缩放由过滤器图的scale步骤处理 |
| libswresample | WavWriter和S04 AudioDecode用它统一声音规格；S03音频转换在过滤器图中安排 |
| libavfilter | S03的TranscodeTrack接出裁剪、时间调整、帧率、尺寸和音频转换步骤 |
| Windows WASAPI/COM | S04的WasapiOutput打开默认输出设备，提交PCM并管理接口寿命 |
| C++线程、锁、条件变量 | RecordingController管理后台任务；AudioPlayer协调生产/消费、有限队列和暂停停止 |

## 建议从这些入口看

- [S01串讲](FEATURES_S01_S04_WALKTHROUGH.md#s01)对应[MediaProbe.cpp](../src/media/probe/MediaProbe.cpp)。
- [S02串讲](FEATURES_S01_S04_WALKTHROUGH.md#s02)对应[VideoFrameExport.cpp](../src/media/video/VideoFrameExport.cpp)。
- [S03串讲](FEATURES_S01_S04_WALKTHROUGH.md#s03)先看[Transcode.cpp](../src/export/Transcode.cpp)，再到TranscodeTrack和Mp4Output。
- [S04串讲](FEATURES_S01_S04_WALKTHROUGH.md#s04)先看[AudioPlayer.cpp](../src/playback/AudioPlayer.cpp)，再到AudioDecode和WasapiOutput。

窗口入口在[main.cpp](../apps/desktop/main.cpp)，命令分发在[AudioCommands.cpp](../apps/cli/AudioCommands.cpp)。S00/S01/S04/S05有窗口入口，S02/S03目前通过CLI；S编号是开发顺序，每次运行会按自己的入口进入对应功能。

本轮只重写项目自有注释和教学说明，第三方SDK、二进制库及legacy备份保持原样，S05基础预览已实现，详见[视频预览](VIDEO_PREVIEW.md)。学习笔记与源码阅读副本同步更新，继续由Git忽略。实际验证见[本轮记录](worklogs/2026-11-14-natural-comments.md)。
