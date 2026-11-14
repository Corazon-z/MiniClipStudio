# MiniClipStudio 当前产品入口（2026-11-14）

**本次更新：S05闪白修正与手动区域录屏（2026-11-14）**：重绘改为内存双缓冲，避免先刷浅色/黑色再画图。主窗“拍摄 / 区域录屏…”可选择无声区域，拖选后录MP4，Ctrl+Shift+F9停止保存；摄像头麦克风保留。默认Debug/Release已生成，16组回归通过；实际窗口观感/拖选与长录待人工。使用见[区域录屏](docs/SCREEN_RECORDING.md)，根因/证据见[开发记录](docs/worklogs/2026-11-14-preview-flash-region-recording.md)。

**S06当前入口（2026-11-14）**：S06基础摄像头功能已实现：主窗“拍摄 / 区域录屏…”选择摄像头来源或`--camera`，支持真实设备/模式、连续预览、可选麦克风、倒计时、H.264/AAC MP4与停止保存。使用/原理见[摄像头录制](docs/CAMERA_CAPTURE.md)，验证与限制见[S06工作记录](docs/worklogs/2026-11-14-s06-camera-capture.md)。Debug/Release及14组回归通过，真实短录已重解；可见GUI、听感与长录仍待人工验收。

本机学习资料已整合为六册，统一入口：`local-notes/六册学习手册/06_项目全貌目录与开发图解.md`。从这里按需阅读功能流程、词典、代码实现、问答与简历、开发复盘和项目总图；旧资料保留。六册由Git忽略，不随GitHub上传。六册首次整理仅改文档；本次S06已实际修改、编译并运行，最新证据见顶部工作记录。

六册S06讲解已再次完善：每册顶部提供直达入口，按完整调用/数据流、词汇与字段、源码逻辑/所有权、代码追问、问题复盘和架构图对应阅读。此次教材修订没有改产品代码或重跑媒体测试，已有运行证据与待人工验收范围分别标明。

S05基础预览已实现：主窗口“打开视频…”或`--preview INPUT`，支持1倍速画面/首音轨、硬件音频主时钟、暂停/继续、按秒跳转、停止/重播。使用与逐模块解释见[视频预览](docs/VIDEO_PREVIEW.md)。GUI/听感仍需人工验收，倍速及更广兼容留后续小步。

公共专项参考：[S01—S04功能串讲及S05续篇](docs/FEATURES_S01_S04_WALKTHROUGH.md)，说明参与模块、FFmpeg库与调用过程；[顺着功能读源码](docs/CODE_COMMENT_GUIDE.md)说明源码注释的阅读方式。个人系统学习优先沿上方六册阅读。

S04基础音频播放已实现：窗口“播放音频…”或`--play INPUT --volume 50`，支持暂停/继续、停止、音量和自然结束。使用FFmpeg解码/重采样、8块有界队列和WASAPI共享输出；Debug/Release真实默认设备静音验证通过。VS无参数运行窗口（清空旧S03命令参数）。原理、逐文件入口和待人工验收见[音频播放](docs/AUDIO_PLAYBACK.md)。S05已新增视频预览/同步/seek，具体范围见本页顶部。

S03有限范围CLI已实现：`--export INPUT OUTPUT.mp4`，支持H.264/AAC、无声、尺寸/固定帧率、裁剪、进度/Ctrl+C取消与可靠发布。使用/原理见[转码导出](docs/TRANSCODING.md)，测量与后续实验见[性能流程](docs/PERFORMANCE_WORKFLOW.md)。真实素材观看/试听待用户验收，S05基础播放器范围见本页顶部；全格式转换尚未实现。

2026-11-13技术路线分析：C++/FFmpeg底座继续沿用；Win32为当前教学界面，复杂UI与采集后端按功能需求另行评估。详细对比在本地 `local-notes/商业技术路线分析与对比.md`；本次未执行架构/SDK迁移。

第一次看项目或感觉架构混乱时，先读本地 `local-notes/项目全貌与开发全过程图解.md`：用三张图串起最终目标、当前代码与开发顺序，不要求先阅读全部专项笔记。该资料Git忽略。本次图解整理不新增产品功能。

2026-11-12：用户确认S00录音已验收；S01媒体信息探测已实现。运行后点击“查看媒体信息…”，或使用 `AudioCapture.exe --probe "文件路径"`。功能/原理/验证见[媒体探测说明](docs/MEDIA_PROBE.md)。

S02已实现CLI视频首帧PNG导出及全段顺序解码计数，使用 `--video-frame INPUT OUTPUT.png`。使用与边界见[视频帧导出](docs/VIDEO_FRAME_EXPORT.md)。本轮按用户选择先完成S02，S03当前状态见本页顶部。

完整实现顺序：[S00—S18功能路线](docs/FEATURE_IMPLEMENTATION_ORDER.md)。当前已到S06基础摄像头预览与录制；S00由用户报告验收，S01—S05已有自动验证，人工窗口/听感及长时验证仍按各阶段范围补齐，再确定后续兼容、性能或S06最小步骤。后续目录规划不代表功能已完成。

打开根目录 `MiniClipStudio.slnx`，选AudioCapture为启动项目，使用Debug/x64或Release/x64。原AudioCapture解决方案仍可使用；输出固定为AudioCapture/x64/配置/AudioCapture.exe。整个仓库是产品范围，AudioCapture仅是兼容构建项目。

- [完整录音流程](docs/RECORDING_FLOW.md)
- [已建立模块与文件职责](docs/MODULES.md)
- [产品质量、编码与发布策略](docs/PRODUCT_QUALITY.md)
- [本次重构与验证记录](docs/worklogs/2026-11-11-modular-refactor.md)

2026-11-11的重构完成模块目录与录音职责提取，当时尚未实现摄像头、时间轴和AI；当前状态以本页顶部S06说明为准。先完成正确功能再优化；源码带中文说明，本地笔记继续不上传。

已实现功能的逐文件逻辑、关键代码、线程/所有权及错误处理详解：本地 `local-notes/已实现功能与源码逐文件详解.md`。该文件由Git忽略，不随公共仓库上传。

从启动、点击按钮到停止保存的完整调用顺序：本地 `local-notes/已实现功能运行过程详解.md`，逐步解释谁调用谁、在哪个线程执行、数据如何传递及最后如何收尾；仅教学说明，不增加产品功能。

---

# MiniClip Studio

学习驱动的 Windows C++ / FFmpeg API 音视频处理系统。

当前业务源码位于apps/与src/，保留初学者中文分段注释。旧AudioCapture.h仅兼容转发，两个旧cpp仅迁移提示，不再是实现入口；原始legacy保留不变。

学习时可先按本地 `local-notes/从零学习教程.md` 从计算机/C++基础、VS操作到当前WAV实现顺序学习。三份学习资料已重写，旧版在本地历史归档；教程和归档均不上传GitHub。

长期目标：逐步构建商用级音视频处理系统，同时理解并能解释每个阶段的实现原理。

详细开发学习笔记保存在本机 `local-notes/MiniClipStudio开发笔记.md`，该目录不纳入 Git。公共里程碑见 `docs/LEARNING_ROADMAP.md`，实际进度见 `docs/PROGRESS.md`。

C++语法与代码阅读另记于本机 `local-notes/C++学习笔记.md`；开发笔记侧重媒体原理与实现。两份本地笔记均不上传GitHub。

VS初学操作指南：本地 `local-notes/VS编译与验证入门.md`；独立线程练习：`local-notes/examples/thread_basics.cpp`，同样不上传GitHub。

完整实际目录与文件职责见 [模块清单](docs/MODULES.md)，早期[架构规划](docs/ARCHITECTURE_PLAN.md)保留作背景。本地 `local-notes/项目目录与VS运行手册.md` 提供当前源码阅读和根解决方案运行步骤。

## 当前环境
- Visual Studio 2026
- 已核实本机 FFmpeg SDK：`C:\ffmpeg-9.0.2-full_build-shared`；使用 x64，生成后部署 DLL 到输出目录
- 尚未安装 Qt；初期保留 Win32 / CLI
- C++ 基础语法已掌握，现代 C++、线程和音视频底层仍在学习

## 第一里程碑
从既有 DirectShow 麦克风采集实验出发，实现可靠的 `录音 -> 解码(如需要) -> 重采样 -> PCM S16 -> WAV封装 -> 验证`。

已接入设备选择、异步录制/停止、48kHz/S16/双声道 WAV 保存及确定性验证。使用、构建、教学解释和未验证项见 [录音说明](docs/RECORDING_GUIDE.md)。本机正常录音与程序化取消已测，设备异常和GUI完整流程仍需补充人工验收。

## 原则
1. 核心媒体处理直接调用 FFmpeg C API，不用 FFmpeg CLI 代替实现。
2. 每次只完成一个可验证的小功能，并解释关键概念。
3. 不一次生成全部编辑器；先沿用现有 Visual Studio 工程。
4. 保留旧代码，避免破坏已能运行的程序。

## 开始
1. 将现有 AudioCapture.h、AudioCapture.cpp、main.cpp 复制到 `legacy/win32_audio_capture/`。
2. 在 VS2026 中打开原有可运行的解决方案，并确认平台 x64、FFmpeg 版本及依赖路径。
3. 将本目录的 AGENTS.md 放在仓库根目录。
4. 在 Codex 中打开仓库，先执行 `docs/CODEX_TASK_00.md`，不允许立即大改代码。

## 窗口录制退出修复（2026-11-14）

窗口模式现支持Ctrl+C/Ctrl+Break请求正常停止、保存并退出；收到请求后请等窗口自行关闭。Ctrl+Shift+F9仍只停止区域录屏并返回窗口。旧.part草稿不自动恢复；关闭控制台、停止调试或结束进程不等于正常保存。验证与源码解释见[故障记录](docs/worklogs/2026-11-14-console-stop-recording.md)。
