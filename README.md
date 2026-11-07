# MiniClip Studio

学习驱动的 Windows C++ / FFmpeg API 音视频处理系统。

## 当前环境
- Visual Studio 2026
- FFmpeg 头文件、库和 DLL 已在既有 VS 项目中配置；具体版本和路径待检查
- 尚未安装 Qt；初期保留 Win32 / CLI
- C++ 基础语法已掌握，现代 C++、线程和音视频底层仍在学习

## 第一里程碑
从既有 DirectShow 麦克风采集实验出发，实现可靠的 `录音 -> 解码(如需要) -> 重采样 -> PCM S16 -> WAV封装 -> 验证`。

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
