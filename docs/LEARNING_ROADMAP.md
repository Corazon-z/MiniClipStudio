# 学习路线与工程里程碑

## M0：环境与代码理解
- 核对 VS2026 工程、FFmpeg 版本、x64、包含目录、库目录、运行时 DLL。
- 理解 Win32 消息循环、CreateThread、AVFormatContext、AVPacket、av_read_frame、资源释放。
- 修正概念：extern "C" 用于 C 链接约定，不是支持重载；volatile 不是线程同步。

## M1：可靠 WAV 录音
- DirectShow 设备选择和打开。
- 读取实际输入编码、采样率、采样格式、声道布局。
- 按需解码成 AVFrame，使用 libswresample 转换到目标 PCM S16。
- 使用 WAV muxer 保存，正确写 header/trailer。
- 正确停止线程，检查输出时长、大小、音质。

## M2：媒体探测与解码
- 自己实现媒体信息探测；理解容器、流、codecpar、packet、frame。
- 解码音频和视频，理解 PTS/DTS/time_base。

## M3：基础音视频处理
- 音频重采样、声道布局与音量；视频缩放和像素格式转换。
- 单片段裁剪与编码封装。

## M4：播放与同步
- 音频输出、视频帧显示、缓冲队列、时钟和 A/V 同步。

## M5：编辑器
- 项目模型、Clip/Track/Timeline、片段拼接、背景音乐、字幕和导出任务。
- 需要复杂 GUI 时再评估 Qt 6。

## 时间安排
以阶段验收而非强制 7 天完成为准。每次新增一项功能，都要能讲清数据流、内存所有权、线程行为和错误处理。
