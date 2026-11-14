# 2026-11-14：用连贯说明重写注释，串起S01到S04

## 这次改了什么

用户反馈前一轮的“专业／白话”标签式注释生硬，希望直接说明一个库/模块是什么、在实现哪个功能时负责什么，函数说明也要连贯，并把S01到当前S04的参与库、函数和逻辑讲清楚。

重新阅读根约束、README、已有代码和配置后，重写44个自有头/源文件。文件头交代所属功能、调用入口、合作模块及所用第三方库；函数前接着讲谁调用、处理什么、结果交到哪里；重要代码段说明实际调用顺序。删去标签和同义重复，保留资源所有权、格式/时间单位、错误传播、取消/等待及真实join等维护信息。不是仅删掉标签，也没有给每句话机械添加“这个函数”。

范围仍为apps、src、tests/integration与旧工程路径3个兼容文件；第三方SDK、二进制库及legacy原始备份不改。兼容cpp仅说明迁移位置，不把它们算成实际功能实现。

新增[功能串讲](../FEATURES_S01_S04_WALKTHROUGH.md)：每阶段先列窗口/CLI入口，再说明本地模块、FFmpeg库、核心函数和具体数据流，并配调用图。明确S编号是开发顺序，S03不会先运行一遍S01和S02入口；目前S00/S01/S04有窗口入口，S02/S03通过CLI。

## 核对说明与代码

两类名称分开介绍：libavformat/libavcodec等是第三方工具库，MediaProbe/TranscodeTrack/AudioPlayer等是项目组织工具完成任务的模块，头文件与cpp不必各自生成独立库。

交叉核对修正两处串讲的表述：S02的EAGAIN来自局部send内部的avcodec_send_packet；收到画面后先校验，decodedFrames仍为0时处理首帧，成功后才累加。程序原有顺序没有修改。PTS/DTS/CFR以及过滤图入口buffersrc/出口buffersink在用到时补上中文含义，API尽量使用可直接搜索的完整名称。

本轮开始另存2026-11-14源码全文与词法基线。最终44个C++文件的注释外记号与本轮修改前一致，6个工程配置和3个legacy文件指纹一致。44个文件均有开头说明，未残留原“专业/白话/用途/流程”标签。词法工具仍是辅助检查，不代替编译器。

## 实际编译和测试

VS2026 MSBuild实际生成产品与独立测试，两种配置均成功。使用命令行OutDir/IntDir覆盖输出位置，每个项目各有独立目录，没有改vcxproj或SDK配置：

```text
build/natural-comments/Debug/product/AudioCapture.exe
build/natural-comments/Release/product/AudioCapture.exe
build/natural-comments/Debug/tests/LifecycleTests.exe
build/natural-comments/Release/tests/LifecycleTests.exe
```

构建确认退出后执行已有回归。Debug/Release的原音频转换、S01、S02、S03、S04共10份报告均通过：

| 验证 | Debug报告（workspace下） | Release报告（workspace下） |
|---|---|---|
| 录音转换/数值/错误 | `audio-check-1794638433664109900/report.json` | `audio-check-1794638433784769100/report.json` |
| S01探测/生命周期 | `probe-check-1794638433811869600/report.json` | `probe-check-1794638433866874500/report.json` |
| S02像素/解码/排空 | `video-check-1794638433680092500/report.json` | `video-check-1794638433801910200/report.json` |
| S03内容/时间/裁剪/取消/发布 | `export-check-1794638433762080300/report.json` | `export-check-1794638433856124800/report.json` |
| S04 PCM/真实设备静音/暂停/停止/重启/CLI取消 | `playback-check-1794638433988802500/report.json` | `playback-check-1794638477685699900/report.json` |

S04显式运行--with-device，两版本都实际使用WASAPI默认设备，设备检查依次运行。未新增针对注释本身的产品单元测试。回归在一定并行负载下执行，不据运行耗时作新的性能结论。

代码块、文档链接与阶段锚点、源码登记、当前逐行副本/版本指纹一并检查；Git差异空白检查通过。本轮没有新的媒体算法修复或新功能，GUI操作/真实听感/长时/设备热切换沿用原待验收状态，不因改说明就记为通过。

## 文档与学习入口

- FEATURES_S01_S04_WALKTHROUGH.md为新的总入口；CODE_COMMENT_GUIDE.md改为自然叙述，并链接四阶段。
- README、学习路线、四份专项文档补入口；PROGRESS记录本轮工作。
- 本地逐文件/运行过程、两份学习笔记、教程入口、Q82—Q84与53文件逐行副本、56文件指纹同步。本地笔记和修改前源码基线仍被Git忽略，不主动暂存上传。

下一步每次选一个阶段，从入口沿真实函数名跟踪。三题答案在本地Q82—Q84，练习/理解仍待用户复述：

1. S01为什么关闭输入后还能排报告？
2. S03的解码、过滤、编码和封装各交给谁？
3. S04为什么解码完还要等设备缓冲消费完？
