# 新 ChatGPT 聊天交接说明

请作为我的 C++ / FFmpeg 音视频开发学习导师，帮助我以项目驱动方式完成 MiniClip Studio。

## 我的背景
- 中山大学计算机科学与技术专业，正在学习 DSP、音视频处理和 FFmpeg。
- 未来希望从事音频工程、音乐技术、AI 音乐相关工作，并计划积累工作经验后申请相关硕士。
- 当前 C++ 只会基本语法，现代 C++、多线程、内存管理、FFmpeg 底层还不熟悉。
- 使用 Visual Studio 2026，FFmpeg 已在既有 VS 项目中配置，尚未安装 Qt。

## 当前已有程序
- AudioCapture.h：声明 StartAudioCapture / StopAudioCapture。
- AudioCapture.cpp：avdevice_register_all、av_find_input_format("dshow")、硬编码 DirectShow 麦克风名、avformat_open_input、av_read_frame、打印 AVPacket size、av_packet_unref、avformat_close_input。
- main.cpp：Win32 窗口、按钮、CreateThread、WaitForSingleObject、停止与关闭窗口逻辑。
- 尚未保存 PCM/WAV；使用 volatile bool 跨线程控制，需要改进。

## 项目目标
基于 C++ 和 FFmpeg C API 开发原生 Windows 音视频处理系统，逐步实现：WAV 录音 → 媒体探测/解码 → 重采样/缩放 → 转码封装 → 播放与同步 → 时间轴剪辑/拼接/混音/字幕/导出。

## 学习方式
不要一次性给我生成整个系统。每次讲清 C++ 概念、FFmpeg API、音视频数据流和工程设计，然后做一个小功能，验证通过后再继续。Codex 负责本地修改和构建，你负责解释、代码审查和答疑。初期沿用 VS 工程与 Win32，不急着引入 Qt 或 CMake。

## 当前下一步
先在 Codex 中做只读工程审查，完整理解已有录音代码，再逐步实现可播放的 WAV 保存。请从这里继续指导我。
