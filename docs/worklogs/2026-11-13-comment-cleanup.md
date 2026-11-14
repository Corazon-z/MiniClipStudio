# 2026-11-13：现有源码注释整理

## 范围与结果

用户要求在每个库/模块文件头写用途，全部函数前用专业术语与大白话解释，并在主要功能代码前说明调用逻辑，删换繁琐重复注释。按现有工程实际文件执行，不新增业务功能或变更架构。

共44个自有`.h/.cpp`：apps的7个、src的27个、tests/integration的7个、AudioCapture旧路径兼容文件3个。文件头均位于文件起始，说明专业职责/白话含义及必要调用关系。函数声明/定义、私有辅助、构造析构、内联查询、default/delete特殊成员与主要lambda补齐说明。长函数按输入准备、关键API推进、结束/失败清理分段解释。

保留实际格式/planar、样本容量与有效数量、linesize、PTS/DTS/时间基、所有权与借用寿命、错误传播、线程/条件变量/真实join。删换逐行重复语法、重复职责与过时说明。修正Job由控制器持有、worker借用，以及AudioPlayer实际Wait条件的说明；不修改相关逻辑。

交叉复查另明确两处术语：WriteFile核对完整写入不等于已显式刷新缓冲（Finish才调用FlushFileBuffers）；暂停分支中的Stop明确写为IAudioClient::Stop，避免与AudioPlayer::Stop结束会话混淆。

不编辑第三方FFmpeg SDK、`.lib/.dll`或legacy原始备份。两工程、SDK属性、宏、函数接口、字符串和业务代码不变。未暂存/上传学习笔记或媒体，也未结束其他进程。

## 核查方法

修改前在Git忽略的本地验证记录保存44个源文件原文与词法记号、6个配置哈希、3个legacy哈希。整理后逐一核对：44个C++非注释记号序列完全一致；6个配置和3个legacy指纹一致。所有44个文件第一行均为用途注释，没有新增行末反斜杠注释续行。`git diff --check`通过。

本地词法工具识别普通/字符/raw字符串，避免把字符串内的`//`等误删；它只是辅助检查，不替代C++编译器。注释改变源码行号，带行号教材/指纹与调试构建也需刷新。

## 实际构建与遇到的问题

使用VS2026 MSBuild，C++20/x64和原FFmpeg配置。首次根Debug构建：全部源码可编译，LifecycleTests生成成功，但产品链接报`LNK1168`，无法写原AudioCapture/x64/Debug/AudioCapture.exe。只观察到目标写入被占用，未确认具体占用方，不据此终止任何进程。

改用命令行`/p:OutDir=... /p:IntDir=...`指定独立目录，未编辑vcxproj。产品Debug/Release随后均实际生成成功：

```text
build/comment-review/Debug/product/AudioCapture.exe
build/comment-review/Release/product/AudioCapture.exe
```

测试分别使用`build/lifecycle/Debug/LifecycleTests.exe`、Release对应路径，均已重新生成。以下回归使用独立目录新产品，等待构建确实退出后才执行；未拿旧EXE当本次结果。

编辑辅助过程遇UTF-8 BOM首行定位、受限测试源码写入、缩进子串重复匹配问题：使用utf-8-sig与原文备份、授权范围的提升写入、行首锚点/唯一定位/重复执行保护解决。每次后仍核对非注释记号，无业务代码修复或新增功能。

## 回归证据

已有五组脚本在两种构建下均通过；没有新增针对注释实现的产品单元测试。

| 验证 | Debug报告（workspace下） | Release报告（workspace下） |
|---|---|---|
| 录音转换/数值/错误 | `audio-check-1794573818938034500/report.json` | `audio-check-1794573864307056100/report.json` |
| S01探测/生命周期 | `probe-check-1794573818888277000/report.json` | `probe-check-1794573864302634300/report.json` |
| S02像素/解码/排空 | `video-check-1794573818963661000/report.json` | `video-check-1794573864329746200/report.json` |
| S03转码/内容/时间/取消/发布 | `export-check-1794573818928749800/report.json` | `export-check-1794573864305523100/report.json` |
| S04 PCM/真实设备静音/暂停/停止/重启/CLI取消 | `playback-check-1794573818981607000/report.json` | `playback-check-1794573869878822100/report.json` |

S04使用`--with-device`，没有跳过真实WASAPI；测试静音，不代替人工听感。本轮未重新操作GUI、录制真实麦克风或测性能，不把旧的待人工验收项改为通过。注释整理不是性能优化成果。

## 文档与下一步

新增CODE_COMMENT_GUIDE.md并更新README、学习路线和进度。本地两份学习笔记、逐文件/运行过程、Q79—Q81、全部44个自有源码的逐行副本和56文件版本指纹同步；本地资料继续Git忽略。源码副本含原先未收录的两个空兼容cpp，明确其不参与编译。

下一步按阅读指南选一个入口，先复述分工再沿代码追踪；功能仍为S04，继续其人工验收，不自动进入S05。若VS正常输出仍被占用，应先正常排查关闭相关运行再重新生成，以刷新调试行号。

理解检查（答案见本地Q79—Q81）：

1. 文件头、函数前、功能块前的说明分别回答什么？
2. 为什么所有权与线程说明比逐行翻译赋值更值得保留？
3. 非注释记号相同，为什么仍重新编译并运行验证？
