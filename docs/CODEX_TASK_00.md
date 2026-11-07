# Codex Task 00 — 只读工程审查与教学

请先不要修改任何源代码或工程配置。

背景：我使用 Visual Studio 2026，已有 FFmpeg SDK 配置，尚未安装 Qt，C++ 只掌握基本语法。已有 Win32 + DirectShow 录音实验，使用 avformat_open_input 和 av_read_frame 读取音频 AVPacket，但没有保存文件。

任务：
1. 检查现有解决方案/项目配置、FFmpeg 版本与库、x64 架构，记录实际发现，不能编造路径或版本。
2. 逐文件解释 AudioCapture.h、AudioCapture.cpp、main.cpp；画出从按钮点击到录音停止的调用流程和数据流。
3. 讲解指针与二级指针、extern "C"、AVFormatContext、AVInputFormat、AVStream、AVPacket、av_packet_unref/free。
4. 找出 volatile bool、阻塞读取、WaitForSingleObject 超时、设备名硬编码、错误路径释放等风险。
5. 说明如何确认设备实际采样率、样本格式、声道布局、编码格式；说明为什么不能直接把 packet data 当作 WAV PCM。
6. 给出实现 48kHz / S16 / stereo WAV 的最小分步计划，包含必要时解码、重采样、PCM 编码、WAV 封装、停止与验证。
7. 给我 3 道检查理解的小题，等待我确认后才开始改动。

输出：环境审查结果、逐段讲解、问题清单、最小修改计划、问题。若无法访问本机 Visual Studio/SDK，请说明需要我提供什么，不要假装已检查。
