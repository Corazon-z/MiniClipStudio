# MiniClipStudio 音频录制从零到文件的完整流程

2026-11-11，按重构后的源码解释。整个仓库是产品，当前只实现其中录音链路。

## 1. 计算机怎样得到声音

声音是随时间变化的压力。麦克风及音频设备将信号采样、量化，通过驱动交给Windows。采样率描述每声道每秒采样数，样本格式描述如何保存幅度，声道布局描述每条信号含义。文件不是从设备里直接“拿到一个WAV”，程序必须读取和处理数据。

## 2. 程序启动

用户在VS打开根目录MiniClipStudio.slnx，选AudioCapture为启动项目、Debug/x64。apps/desktop/main.cpp的wmain执行：有参数交给apps/cli的实验命令；无参数注册窗口类、创建控件、设置定时器，再进入Windows消息循环。

GUI线程是处理点击和绘制的线程，不能直接长期采集。WM_CREATE发起后台设备枚举，DeviceCatalog使用FFmpeg dshow枚举清单，只保留音频设备并复制名称后释放清单。

## 3. 点击开始

StartRecording读取选中设备，弹出保存对话框，确认新的文件路径。参数复制到RecordingJob，避免worker引用局部路径数组。RecordingController拥有Job和thread，启动worker；窗口只显示状态。

## 4. worker打开输入

AudioRecorder.cpp中的RunAudioCapture检查是否预先取消，创建InputResources，配置取消回调、非阻塞读取与50ms设备缓冲请求，再调用avformat_open_input。实际格式由设备协商，不假定48000。打开完成再次检查取消，避免启动期间丢掉停止请求。

找到第一条音频流，记录stream_index，根据codecpar中的codec_id找到解码器，将参数复制到AVCodecContext并open2。资源由worker局部RAII对象拥有，GUI不能并发释放。

## 5. 准备WAV输出

WavWriter::Open建立wav输出上下文、PCM_S16LE编码器和输出流，设置目标48000Hz/S16/stereo与1/48000时间基，写同目录临时文件header。WAV是容器，PCM是编码表示；这个路径合法且适合当前无损录音，不因商业目标就必须压缩。

## 6. 持续读取和解码

read_frame取得包，不假定packet就是PCM，检查流编号后send_packet。receive_frame可能产出零帧/多帧；循环取出全部可用帧，EAGAIN推进另一侧。非阻塞设备暂无包时等待5ms再检查取消，10秒无音频报错。等待不是同步，不能据此认为线程退出。

## 7. 转换样本

第一帧实际sample_rate/format/ch_layout初始化SwrContext。swr_convert处理采样率、样本类型和声道/排列；extended_data传递多平面，不漏右声道。容量根据内部延迟估算，produced才是有效每声道样本数。pts取已有样本累计数，转换完成再累加。

## 8. 编码与封装

目标AVFrame交PCM编码器，取输出AVPacket，换算时间基后写WAV。PCM编码虽不压缩，仍确定小端16位字节表示。每个包用完释放引用，包/帧对象按所有权析构；不会将设备字节直接改后缀。

## 9. GUI观察进度

worker写atomic样本计数，GUI定时读取，除48000得到有效音频秒数。普通result/devices在线程join后才读。RecordingController::TryJoin只零等待检查，未结束保留所有者，不让UI卡住或重复开第二任务。

## 10. 用户停止或关闭窗口

GUI把cancel置true。worker必须自行检查并退出，atomic不会强杀设备调用。关闭窗口先记关闭意图，继续消息循环等待。驱动打开/关闭可能耗时，没有硬性截止保证。

## 11. 排空与发布

解码器空包取剩余帧 → 重采样器空输入取延迟样本 → 编码器空帧取剩余包 → 写trailer → 关闭IO → 不覆盖方式发布最终文件。无样本不发布。只有Finish成功才报告保存成功，停止后写盘失败仍是失败。

## 12. 回收与重启

worker返回结果、局部资源析构。控制器确认结束后join，GUI读结果、清空Job、恢复按钮；若请求关窗再销毁。join不是取消，ClearFinished拒绝丢弃尚未join的任务。异常清理不是文件成功证明。

## 13. 对照文件

- apps/desktop/main.cpp：窗口与事件。
- apps/cli/AudioCommands.h/.cpp：测试/命令行参数。
- src/application/RecordingController.h/.cpp：线程与会话所有权。
- src/capture/DeviceCatalog.h/.cpp：设备目录。
- src/capture/audio/AudioRecorder.h/.cpp：采集/解码编排。
- src/media/ffmpeg/FfmpegSupport.h/.cpp：错误、路径与资源。
- src/media/mux/WavWriter.h/.cpp：转换、PCM编码、封装与发布。

源码注释保留并随迁移修正。具体语法、引用和析构详见本地C++教程。理解题：UI为什么不能执行read循环？编码与封装有什么区别？为什么worker结束也可能是失败？
