# S01 媒体信息探测：使用与实现

S06当前入口（2026-11-14）：基础摄像头预览/可选麦克风MP4已实现，见[摄像头说明](CAMERA_CAPTURE.md)。本篇原有功能仍按自己的范围使用；S06复用不表示它新增所有摄像头控制、HDR或设备恢复。当前学习优先读本地六册，历史段落按日期理解。

先想弄清这个功能由哪些模块、库和函数配合完成，可以从[功能串讲的S01一节](FEATURES_S01_S04_WALKTHROUGH.md#s01)开始，再回来查本篇的使用和边界。

2026-11-12。S00由用户报告已验收；这里新增本地文件探测，不修改输入，不进行全程播放/转码。

## 使用

在根MiniClipStudio.slnx生成AudioCapture（Debug/x64或Release/x64）。无命令参数运行后，点击“查看媒体信息…”，选择文件，后台读取，结果显示在可滚动/复制的只读区域，同时打印到控制台。录音/枚举/探测互斥；即使没有麦克风，枚举结束后仍可选择文件。探测中关闭窗口会请求取消，真实结束后再关闭。

命令行：

```powershell
cd C:\Projects\MiniClipStudio
& .\AudioCapture\x64\Debug\AudioCapture.exe --probe "C:\你的素材\视频.mp4"
```

路径换成真实文件。失败返回1，用法错误返回2，成功返回0。不接受网络URL，本阶段仅普通文件；媒体内部引用也限定file协议。不是全格式支持承诺。

## 输出解释

- Container/容器：装音轨和画面的外壳，例如WAV/AVI；不等于里面的编码。
- Stream/轨道：文件里的一路内容，逐条列出，包括音频、视频或其他类型。
- Codec/编码：样本/画面如何保存，不靠后缀猜测。
- Duration/时长：有证据才显示秒数，未知显示“未知”。流时长和总时长不保证相等。
- Bit rate/码率：平均每秒多少比特，不是采样率。
- Time base/时间刻度：时间戳一格多长，如1/8000秒。
- Sample rate/采样率、channels/声道、layout/排列、sample format/样本存法。
- 视频宽高、pixel format/像素存法、平均与参考帧率；两个帧率都不单独证明恒定帧率，封面图也单独标记。

## 文件职责

| 文件 | 作用 |
|---|---|
| src/media/probe/MediaProbe.h | 无FFmpeg裸指针的数据报告、取消异常和两个公开函数 |
| MediaProbe.cpp | 打开/探测/抄参数/关闭，以及报告排版 |
| apps/cli/AudioCommands.cpp | --probe命令入口 |
| apps/desktop/main.cpp | 选文件、提交任务、报告显示和换行 |
| RecordingController.h/.cpp | TaskKind明确三类任务；worker执行ProbeMedia分支 |
| tests/verify_probe.py | 直接验证CLI、样本和失败行为 |
| tests/integration/LifecycleTests.cpp | 多流AVI样本、未知/预取消、后台分支和旧生命周期回归 |

## 原理与所有权

本地路径检查 → 绝对路径转UTF-8 → 创建输入上下文 → 设置合作式取消/15秒期限 → open_input → find_stream_info → 按每条流拷贝普通值 → RAII关闭 → 返回MediaInfo → FormatMediaInfo供UI/CLI显示。

find_stream_info可能读取部分包并内部解码帮助识别，不是只看扩展名，也不是完全不读媒体数据；但没有为用户逐帧播放，不能据探测成功证明文件尾部完整或能流畅播放。

cancel指针和ProbeReadState比输入回调使用期更长；输入资源只在工作线程拥有。报告不保存AVStream/codecpar指针，文件关闭后仍能展示。std::optional保存“有数值/没有信息”两种状态，不用0冒充未知。enum class TaskKind比多个bool组合更容易理解。

期限与取消依赖底层检查回调，不是强制杀掉任意磁盘/系统调用。GUI不在按钮函数中读文件，也不超时后丢失线程。

## 验证

```powershell
python tests/verify_probe.py AudioCapture/x64/Debug/AudioCapture.exe build/lifecycle/Debug/LifecycleTests.exe
python tests/verify_audio.py AudioCapture/x64/Debug/AudioCapture.exe
```

探测测试覆盖中文/空格路径、WAV参数与源内容哈希、改扩展名、视频+两音轨AVI、尺寸/帧率/像素、缺失/目录/损坏输入、无效参数、未知字段和预取消、控制器后台探测。AVI用FFmpeg C API在测试EXE中生成，独立测试main不加入产品。真实GUI对话框交互另由用户手工验收，不混同控制器自动测试。

限制：不做联网探测、所有元数据/章节/旋转色彩信息提取、完整损坏扫描、VFR判定或自动重试。当前S01交付基础容器/逐流信息与界面/CLI入口，更多规格按实际需求增加。
