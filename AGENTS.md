# MiniClip Studio — Codex 工程与教学约束

## 用户背景
用户使用 Windows + Visual Studio 2026；既有 Visual Studio 项目已配置 FFmpeg SDK；未安装 Qt；C++ 仅掌握基础语法。项目的第一目标是边开发边理解 C++、FFmpeg 与音视频原理，而不是快速堆功能。

## 技术决策
- 初期沿用原有 VS 工程和 Win32 界面，可添加 CLI 小实验。
- 媒体处理必须使用 FFmpeg C API（libavformat、libavcodec、libavutil、libswresample 等），不以 FFmpeg 命令行替代核心功能。
- 后期再考虑 CMake、Qt 6、播放器、时间轴。
- 保留原始程序于 `legacy/win32_audio_capture/`，不可覆盖。
- 不假定 FFmpeg 的具体版本、SDK 路径、ABI 或 API 可用性；先检查。

## 每次任务的强制流程
1. 阅读现有代码与项目配置，列出事实和不确定项。
2. 用中文讲解本次涉及的 C++ 概念、音视频数据流和 FFmpeg API。
3. 给出最小改动计划，等用户确认后再做重大结构性修改。
4. 实现一个可验证的功能；解释每个新增类、函数和资源所有权。
5. 实际编译并运行可运行的测试；不能执行时如实报告。
6. 总结修改文件、测试结果、错误、下一步，并提出 3 个理解检查题。

## 代码规范
- 初期代码清晰易懂，避免复杂模板、过度抽象和空壳类。
- `volatile` 不能替代线程同步；跨线程状态使用 `std::atomic` 等机制。
- FFmpeg 分配的资源要正确释放；逐步引入 RAII。
- 不假定 `AVPacket` 是 PCM，也不假定 `AVFrame` 音频数据是 packed。
- 输入设备实际格式需读取，输出目标格式可设置为 48000Hz、S16、stereo；必要时解码和重采样。
- 关注 `av_read_frame` 阻塞、停止和线程退出；不能超时后假装线程已退出。
- 所有重要 FFmpeg 返回值需检查，错误信息可读。
- GUI 线程不能被长时间采集/编码阻塞。

## 文档
维护 `README.md`、`docs/LEARNING_ROADMAP.md`、`docs/PROGRESS.md`，并在每次任务后更新学习笔记。

本地详细学习笔记入口：`local-notes/MiniClipStudio开发笔记.md`。每次开发后记录概念解释、API 与资源所有权、问题根因和解决方法、实际验证证据、性能测量及理解检查题。该目录由 Git 忽略，不得主动暂存或上传；用户要求只读时不写入笔记。区分已实现、已验证、待验证和远期设计，不把计划写成成果。

本地 C++ 学习笔记：`local-notes/C++学习笔记.md`，记录语法、标准库、代码逐段解释、所有权与生命周期、异常和线程知识及练习；开发笔记记录音视频/FFmpeg原理、实现流程和工程取舍。两份笔记互相引用、随任务更新，均不上传GitHub；没有实际执行的练习不得标记为通过。
