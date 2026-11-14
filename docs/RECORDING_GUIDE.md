> 2026-11-11更新：以下保留早期规划/实现说明作历史参考。当前实际目录、源文件位置和状态以MODULES.md、RECORDING_FLOW.md及本次worklog为准；不再只在旧AudioCapture.cpp/main.cpp开发。

S06当前入口（2026-11-14）：基础摄像头预览/可选麦克风MP4已实现，见[摄像头说明](CAMERA_CAPTURE.md)。本篇原有功能仍按自己的范围使用；S06复用不表示它新增所有摄像头控制、HDR或设备恢复。当前学习优先读本地六册，历史段落按日期理解。

# WAV 录音使用与实现说明

日期：2026-11-07。沿用现有 Win32 / VS 工程，原始 legacy 程序未改动。

## 使用

1. 在 VS2026 打开 `AudioCapture/AudioCapture.slnx`，选择 Debug/x64 或 Release/x64，生成后运行。
2. 等待后台枚举，选择麦克风；连接新设备后可以“刷新设备”。
3. 点击“开始录音…”，在保存对话框选择一个新的 WAV 路径。当前主动保护已有文件，不支持覆盖。
4. 录制时可看到已处理音频时长，实际输入编码与解码帧参数打印到控制台。
5. 点击“停止并保存”，等待完成提示及最终路径。关闭窗口也先请求结束后台任务，线程真正退出后才关闭。

目标输出：48000 Hz、PCM S16LE、双声道 WAV。输入保持设备实际协商格式，经解码与重采样转换。当前请求50ms设备缓冲，实际延迟由设备与驱动决定。

短到尚未收到音频样本的录制不会生成最终文件。保存成功前使用同目录 `.part-进程号-时钟值` 临时文件；正常错误路径会清理它，强制结束进程或断电仍可能留下临时文件。原始素材和已有录音不会被覆盖。

工程保留已核实的本机 SDK 路径 `C:\ffmpeg-9.0.2-full_build-shared`，构建后复制该 SDK 的 DLL 到输出目录。换电脑时仍需重新配置 SDK。Win32 构建会明确提示选择 x64，避免误链接 x64 库。

## 为什么这样修改

```text
GUI线程：设备/路径选择 → 启动Job → 定时器零等待探测 → join → 显示结果
                            ↓ 原子取消请求/样本进度 ↑
工作线程：打开设备 → 读AVPacket → 解码AVFrame → swr转换
         → PCM S16LE编码 → WAV写包 → 排空 → trailer → 关闭 → 发布文件
```

同步原则：每次启动创建独立 Job，取消标志初始化为false，工作线程不会将停止请求改回false。只有线程结束并join后，GUI才读取普通结果字段或释放Job。运行中共享的取消和进度使用atomic。

停止原则：读取端设置 `AVFMT_FLAG_NONBLOCK`，没有数据时处理 `EAGAIN` 并等待5ms，再检查取消；连续10秒收不到音频报错。该行为依据SDK声明的源提交中[dshow读取实现](https://github.com/FFmpeg/FFmpeg/blob/946fcce07b/libavdevice/dshow.c)核对，并已在本机设备正常录制路径上验证。中断回调只是额外协助，不能保证驱动打开/关闭总能立即返回。GUI在任务结束前保持忙碌状态，不丢失线程。

封装原则：不假设packet是PCM，统一使用对应解码器得到帧；重采样器从第一帧实际参数初始化。读取 `extended_data` 支持planar，不假设所有声道都在data[0]。流中途改变参数时明确失败，不继续以旧参数处理。

时间原则：输出pts使用累计每声道样本数，编码器time_base为1/48000，写包时换算为输出流time_base。时长显示以实际处理样本为依据，不等同于按钮按下后经过的墙钟时间。

收尾原则：发送空包排空解码器，再排空重采样器、编码器，写trailer并关闭IO，最后发布文件。写盘和收尾失败是失败，即使用户已点击停止，也不会伪装成正常取消。

## 类型、函数与所有权索引

| 类型/函数 | 职责及资源 |
|---|---|
| AudioDevice | 拥有UTF-8设备标识和显示名称字符串，摆脱硬编码 |
| CaptureOptions | 不可变任务输入：设备或测试输入文件、Unicode输出路径、可选录制时限 |
| CaptureResult | 任务结果：成功/取消、实际样本数和信息；没有FFmpeg裸指针 |
| ListAudioDevices | 调用设备枚举API，仅保留音频设备，成功/异常均释放AVDeviceInfoList |
| RunAudioCapture | 串联输入、解码、转换和保存；捕获异常生成结果，在工作线程执行 |
| WriteSyntheticAudio | 生成确定性float planar帧，复用WavWriter验证生产转换路径 |
| InputResources | RAII聚合，拥有输入上下文、解码器和选项字典，析构按配套API释放 |
| FrameDelete/PacketDelete | unique_ptr自定义删除器；让帧/包在离开作用域时调用FFmpeg释放函数 |
| NoSamples | 区分没有样本的正常取消与真实写盘/编码错误；后者不会被取消标志掩盖 |
| NewFrame/NewPacket | 检查分配失败，返回拥有资源的unique_ptr；不使用delete释放FFmpeg对象 |
| WavWriter | 只管理一个输出文件，拥有输出上下文、编码器、重采样器和临时文件；禁止复制 |
| WavWriter::Open | 建立PCM编码器、输出流、IO并写header，失败由析构清理 |
| WavWriter::Push | 校验真实输入格式，首次初始化重采样器，之后检测格式变化 |
| WavWriter::Convert | 根据重采样延迟估算容量，转换、设置实际样本数和pts；空输入用于排空 |
| WavWriter::Send/Receive | 编码send/receive循环，处理EAGAIN，换算时间基并写包 |
| WavWriter::Finish/Samples | 排空并发布完整文件；查询每声道输出样本数 |
| WavWriter析构 | 释放转换/编码/输出资源，未发布则清理临时文件；清理失败写诊断 |
| Check/Utf8/Interrupt | 错误码转可读异常、Unicode路径转换、读取原子取消标志；不拥有上下文 |
| Job | GUI拥有的会话数据；线程借用其地址，join前不销毁 |
| StartJob | 创建std::thread，创建失败恢复空闲并显示错误 |
| Refresh/StartRecording | 发起后台枚举；获取设备、弹出保存对话框、创建录制任务 |
| Poll | 定时器调用，WaitForSingleObject(...,0)探测；确认结束才join和读取结果 |
| Controls/Status/Wide | 仅GUI线程更新控件状态与文本，UTF-8转UTF-16 |
| WndProc | 窗口创建、按钮、定时器与关窗请求，检查关键初始化失败 |
| CommandLine/PrintResult | 无GUI实验入口及明确退出码；取消可返回0，真实失败返回1 |
| wmain | Unicode参数、窗口与消息循环；显式处理GetMessage的-1/0/正数 |

关键C++概念：RAII用析构保障错误路径清理；unique_ptr表达唯一所有权；lambda用于局部处理循环和工作线程入口；std::thread负责线程句柄生命周期。Windows native_handle仅用于零等待观察，不手动CloseHandle或detach。

## 验证方法

解决方案构建（PowerShell，MSBuild位置为本机已检查路径）：

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe' AudioCapture/AudioCapture.slnx /p:Configuration=Debug /p:Platform=x64 /v:minimal /nologo /m:1
```

构建后可运行：

```powershell
python tests/verify_audio.py AudioCapture/x64/Debug/AudioCapture.exe
& ./AudioCapture/x64/Debug/AudioCapture.exe --devices
& ./AudioCapture/x64/Debug/AudioCapture.exe --record 0 3 C:\Projects\MiniClipStudio\workspace\my-recording.wav
& ./AudioCapture/x64/Debug/AudioCapture.exe --cancel-test 0 3 C:\Projects\MiniClipStudio\workspace\my-stop-test.wav
```

录音命令先确保workspace目录存在并选择新的输出名。设备序号通过--devices确认。--record的时限从输入/输出准备完成后开始计算；--cancel-test的等待从工作线程启动算起，包含设备打开时间，适合测启动期间取消。以上命令都在程序内调用FFmpeg C API，不调用ffmpeg命令行工具。

### 2026-11-07 实际结果

- 沙箱内编译曾在链接时出现 `LNK1101 / MSPDB140.DLL`；同一安装在沙箱外成功构建，不据此修改系统DLL或声称VS安装损坏。
- 沙箱内设备枚举失败；沙箱外成功枚举本机麦克风。实际输入为44100Hz、pcm_s16le、双声道。
- Debug/x64、Release/x64解决方案构建成功。测试脚本校验RIFF长度、fmt/data块、48000Hz/S16/双声道、样本数和信号，不假设固定44字节WAV头。
- 44100Hz float planar mono与stereo、16/24位packed mono输入：每个1秒样本目标为48000个每声道样本；stereo测试分别核对左右440/880Hz，避免漏掉第二平面。
- 已有文件哈希不变、缺失/损坏输入、无效输出路径、启动前取消与临时文件清理通过。
- 请求50ms缓冲后，3秒读取窗口得到141600样本，即2.95秒；WAV块解析通过。尚未送达应用的设备缓冲不会被虚构成已录制样本。
- 一次停止测试退出等待约57ms，实际收到2400样本；1秒测试等待包含设备打开耗时，实际音频仅0.05秒。启动期间立即取消约0.97秒后退出，未发布文件。这些是单次本机观察，不是延迟保证。
- GUI设备列表、保存对话框打开、取消返回与空闲关闭已观察；自动化工具未能可靠填写保存对话框，未完成GUI录音全过程验证。

### 仍需人工验收与后续改进

- 实际试听音质、GUI连续快速点击、录制中关窗、设备占用/断开、磁盘满和长时间运行尚未全部验证。
- 驱动打开和关闭可能长时间阻塞；当前保持界面响应并等待真实退出，不承诺硬性停止期限。
- 无数据超时或设备故障会报告失败并清理本次临时输出；异常录音片段恢复尚未实现。
- 当前不处理设备时间戳间隙的补静音、录音暂停恢复、电平表或MP3/AAC输出。
- 转换期间仍为每次处理分配部分帧/包；已去掉逐包日志，缓冲复用优化留待性能测量后做。
- VS编辑器如有未保存的旧源码缓冲，应先比较并保留用户内容，避免覆盖磁盘新实现。
