# MiniClipStudio 当前产品入口（2026-11-11）

打开根目录 `MiniClipStudio.slnx`，选AudioCapture为启动项目，使用Debug/x64或Release/x64。原AudioCapture解决方案仍可使用；输出固定为AudioCapture/x64/配置/AudioCapture.exe。整个仓库是产品范围，AudioCapture仅是兼容构建项目。

- [完整录音流程](docs/RECORDING_FLOW.md)
- [已建立模块与文件职责](docs/MODULES.md)
- [产品质量、编码与发布策略](docs/PRODUCT_QUALITY.md)
- [本次重构与验证记录](docs/worklogs/2026-11-11-modular-refactor.md)

本轮完成模块目录与录音职责提取，未实现摄像头、时间轴和AI效果。先完成正确功能再优化；源码带中文说明，本地笔记继续不上传。

已实现功能的逐文件逻辑、关键代码、线程/所有权及错误处理详解：本地 `local-notes/已实现功能与源码逐文件详解.md`。该文件由Git忽略，不随公共仓库上传。

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
