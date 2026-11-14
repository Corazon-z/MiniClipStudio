# S02：视频解码与首帧PNG导出

S06当前入口（2026-11-14）：基础摄像头预览/可选麦克风MP4已实现，见[摄像头说明](CAMERA_CAPTURE.md)。本篇原有功能仍按自己的范围使用；S06复用不表示它新增所有摄像头控制、HDR或设备恢复。当前学习优先读本地六册，历史段落按日期理解。

先想弄清这个功能由哪些模块、库和函数配合完成，可以从[功能串讲的S02一节](FEATURES_S01_S04_WALKTHROUGH.md#s02)开始，再回来查本篇的使用和边界。

2026-11-12。用户确认按顺序先完成S02，本轮未实现S03整段转码。保留现有VS/Win32；新增可验证的CLI实验，不引入Qt/CMake，不调用ffmpeg命令行。

## 使用与输出含义

打开根MiniClipStudio.slnx，AudioCapture为启动项目，生成Debug/x64。在项目属性→调试→命令参数填写：

```text
--video-frame "C:\素材\输入视频.avi" "C:\素材\首帧.png"
```

路径换成真实输入和**尚不存在**的输出；F5或Ctrl+F5运行。清空命令参数可恢复原GUI。当前S02没有新增GUI按钮；普通CLI同步执行，不经RecordingController，不会阻塞一个同时存在的产品主窗口，因为这个启动分支不建主窗口。

也可在PowerShell运行：

```powershell
cd C:\Projects\MiniClipStudio
& .\AudioCapture\x64\Debug\AudioCapture.exe --video-frame "C:\素材\视频.avi" "C:\素材\首帧.png"
```

报告width/height为解码图像尺寸，decoded_frames为**选中视频轨道顺序解码并排空得到的帧数**，不是压缩包数。首帧时间戳按输入流time_base换为秒，可未知，也不一定从0开始。输出是首个解码输出帧，不承诺是“时间戳0秒的画面”。成功0、处理失败1、命令参数形状不符2。

PNG虽只含首帧，但当前命令会读取到视频结尾，以验证顺序解码与尾部排空；长视频需要等待。仅在这些步骤成功后发布图片。它不是快速缩略图接口，也不是播放器/视频转码器。

## 初学者手工验收顺序（2026-11-13补充）

1. 在VS打开根解决方案，右键AudioCapture设为启动项目，选Debug/x64，生成解决方案并确认无失败。生成失败时先排错，不用旧EXE验证新源码。
2. 右键AudioCapture项目→属性→配置属性→调试→命令参数，填写前文--video-frame的两个真实路径。输出以.png结尾、父目录存在、目标尚不存在；含空格路径保留双引号。应用后Ctrl+F5运行，当前不会显示录音窗口。
3. 等到报告PNG saved、width/height及decoded_frames，不能因临时文件出现就当完成。当前先解码整段再发布，耗时不等于视频实际时长。
4. 用图片查看器打开PNG，核对尺寸、上下方向和颜色，没有条纹/错位；普通未旋转素材可对照原视频开头画面。首帧原本全黑时，黑图不自动算错误；本实现不应用视频旋转标签或SAR校正，不能拿自动旋转播放器显示作为无条件一致保证。
5. 不改输出路径重跑，预期拒绝覆盖且原PNG不变；这是保护行为，第二次正常导出应换新文件名。
6. 在项目根PowerShell运行verify_video.py（命令见实际验证节），成功应输出PASS和report.json位置。它检查已知输入帧数/像素与错误行为，真实素材的decoded_frames>0本身不是完整正确性证明；不要用总时长×参考帧率猜准确帧数。

验收分别记录：配置/生成结果、命令、控制台报告、PNG尺寸与肉眼检查、失败保护和自动报告。恢复GUI时清空VS调试参数。生成、人工看图和自动断言是不同证据。

## 概念到数据流

像素是图像的小格，每格要用数值表示颜色。RGB24通常每像素按R/G/B三个8位分量；YUV420把亮度和色度分平面存储，不能按RGB字节解释。linesize（行跨度）是从一行到下一行的内存步长，可能含填充或方向信息；不是所有格式都等于width×3。

数据流：

```text
RunAudioCommands → ExportFirstVideoFrame
 → 本地文件打开/流探测 → 选择第一条非封面视频流 → 建立解码器
 → av_read_frame读包 → send_packet / receive_frame取得画面
 → 第一帧：sws_scale按data+linesize转换RGB24 → PNG编码 → 写临时文件
 → 后续帧：检查尺寸/格式并计数 → 输入EOF → 空包排空解码器
 → 确认实际有帧、未取消 → 关闭/发布PNG → 返回报告
```

解码器可能延迟输出。例如含B帧的视频，部分画面要等待其他参考帧；输入EOF后还必须send_packet(nullptr)并receive到解码EOF，不能用输入包数猜画面数量。

libswscale负责像素格式/颜色矩阵范围转换；libavcodec的PNG编码器把RGB样本变为真正PNG包。单张PNG包直接写入文件，不需要WAV式音视频容器trailer；但PNG编码器仍需结束输入并取完输出。

## 文件、函数与所有权

| 文件/函数 | 大白话职责与资源 |
|---|---|
| src/media/video/VideoFrameExport.h | 自有报告、取消异常、公开同步函数；不向调用方暴露FFmpeg资源 |
| ExportFirstVideoFrame | 组装完整任务，局部InputResources拥有输入和视频解码器 |
| CheckCancelled / InterruptVideo | 阶段检查可抛取消异常；库回调只返回中断意图，不抛异常 |
| ValidatePicture | 检查尺寸、当前支持格式、HDR与解码错误，不让未支持数据静默输出 |
| ColorMatrix | 把输入颜色矩阵标记映射为swscale系数选择 |
| ConvertToRgb | 分配自有RGB帧、设置矩阵/范围，按真实行跨度转换；Scale智能指针释放转换器 |
| EncodePng | 局部Codec智能指针拥有PNG编码器，帧包由既有Frame/Packet所有者管理 |
| PendingPng | 独占CREATE_NEW创建临时文件；Write写PNG包，Publish刷新/关闭/拒绝覆盖发布；失败析构清理 |
| CodecDelete / ScaleDelete | 告诉unique_ptr分别用avcodec_free_context/sws_freeContext释放 |
| apps/cli/AudioCommands.cpp | 新增命令参数分支与自有结果显示；不创建新媒体引擎 |
| tests/integration/VideoFixtures.h/.cpp | C API生成8帧MPEG4（max_b_frames=2）测试输入，不编进产品 |
| tests/verify_video.py | 生成方向/行填充样本，解析PNG并验证真实像素、帧数和失败行为 |

InputResources与输入流借用关系同S01。解码帧只在receive循环内借给转换器；首帧转换完后可unref。只保留当前帧/包，不把整段视频帧堆进vector。报告返回普通数值和自有路径，文件关闭后仍能用。

PendingPng拥有系统句柄，禁止复制；CREATE_NEW失败不会取得文件所有权，不删除别人的同名文件。成功后若未发布，析构关句柄并尝试删临时文件；发布失败不会返回成功。该机制不是断电恢复事务，仍需未来更系统的恢复设计。

## 当前支持范围与限制

- 本地普通文件；第一条非attached picture（封面）视频流，忽略其他音轨/视频流。PNG/BMP等静态图片也可能由FFmpeg表示为一帧视频。
- 软件8位YUV420P/YUVJ420P、RGB24、BGR24、GRAY8；不支持的像素格式明确报错。限制边长不超过16384、像素不超过6400万；不是全部格式/所有分辨率的兼容承诺。
- 对常见颜色矩阵/范围做转换；缺少矩阵信息时用swscale默认。HDR PQ/HLG明确拒绝，不做色调映射；不做ICC/完整色彩管理。
- 保留编码图像方向和尺寸，不应用display matrix旋转、不校正非方形像素SAR；导出的画面可能与自动旋转播放器展示不同。不要把BMP存储行方向测试误认为已支持视频旋转标签。
- 有取消参数与回调，测试覆盖预取消；当前CLI没有交互式取消按钮/信号处理。中途取消在真实长文件/底层阻塞上的表现未系统验收，不提供硬期限。
- 解码器报告错误会失败；某些截断/封装损坏可能只表现为正常EOF，不能把顺序读到EOF称为完整损坏鉴定。没有真实设备录制/播放器观看验收。

## 实际验证

Debug/x64与Release/x64构建成功，两套视频/音频/探测及生命周期检查通过。视频报告位于本机忽略目录：

- Debug：`workspace/video-check-1794483188078033700/report.json`
- Release：`workspace/video-check-1794483255435441900/report.json`

测试包含中文/空格路径、上下方向两种BMP、18像素宽导致的行填充、红绿蓝通道实际PNG像素、YUV中性灰、两个音轨跳过、两帧视频及8帧B帧延迟排空；PNG重探测、输入内容哈希不变；无视频/损坏/缺失/目录输入、已有目标不变、错误后缀/路径、预取消和临时清理。PNG验证包含chunk CRC、zlib解压、五种行过滤重建，不只是检查文件存在。

```powershell
python tests/verify_video.py AudioCapture/x64/Debug/AudioCapture.exe build/lifecycle/Debug/LifecycleTests.exe
python tests/verify_audio.py AudioCapture/x64/Debug/AudioCapture.exe
python tests/verify_probe.py AudioCapture/x64/Debug/AudioCapture.exe build/lifecycle/Debug/LifecycleTests.exe
```

未做性能基准；没有把测试执行时限当作性能承诺。后续最小步骤是用户用真实素材运行/核对首帧，再设计S03无声视频转换及时间戳/可靠导出，不在本轮继续堆功能。
