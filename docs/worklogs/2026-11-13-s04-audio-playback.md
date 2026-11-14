# 2026-11-13：S04基础音频播放

## 请求与实现范围

用户要求继续S04，代码强可读/可维护并完善文档。先读AGENTS、README、功能顺序、现有录音/控制器/桌面/CLI与两工程配置；核实VS2026/v145/C++20、本机FFmpeg9.0.2共享SDK及Windows SDK。S04之前播放相关目录仅有说明，没有设备输出代码。

本轮沿用现有方案、Win32与FFmpeg C API。新增解码、会话、WASAPI三个小模块和GUI/CLI入口；没有搬工程、替换录音控制器或引入Qt/CMake。重大架构迁移未执行。保留legacy、用户媒体/PNG/旧part文件及原有未提交修改，不暂存或上传本地笔记。

已实现本地首音轨播放、48kHz/float32/stereo统一PCM、1024帧块/最多8块队列、暂停/继续、0—100%软件音量、明确停止/自然结束/错误收尾、GUI非阻塞轮询与关窗先停止、CLI按键和Ctrl+C/Ctrl+Break。

## 文件和所有权

- `src/media/audio/AudioDecode.h/.cpp`：同步文件/解码/重采样/分块及取消，拥有FFmpeg资源。
- `src/platform/windows/output/WasapiOutput.h/.cpp`：默认端点、共享缓冲、padding、写入增益、COM/事件RAII；全部在消费线程使用。
- `src/playback/AudioPlayer.h/.cpp`：两线程与有限队列、mutex/条件变量、原子停止/音量、状态副本、真实join。
- `apps/cli/PlaybackCommands.h/.cpp`与AudioCommands分发，`apps/desktop/main.cpp`新增控件/对话框/轮询/关闭处理。
- `tests/integration/PlaybackTests.h/.cpp`、LifecycleTests测试命令及`tests/verify_playback.py`；两vcxproj/产品filters登记文件，ole32用pragma链接。
- README、路线、模块、性能流程及AUDIO_PLAYBACK.md；本地F20、运行21、两份笔记、教程/VS/目录/图解、Q69—Q78、I61—I68、岗位对照和源码快照。

录音已有InputResources/Frame/Packet/Check复用，不为所有目录造空类。暂停保留会话以继续，结束/停止/失败释放；原路线“暂停/关闭释放设备”明确为暂停停止输出并保留位置，最终结束才释放。共享模式不独占设备。

## 设计与问题复查

1. 播放速度由设备消费而非CPU解码决定；有界队列产生背压。只填容量-padding空位，尾部必须等设备消费完，提交样本不等于已经听到。
2. 生产端按实际AVFrame格式转换，支持固定mono/stereo、8—192kHz；限制输入单帧/转换暂存大小，中途变格式/多声道明确报错。PCM队列约64KiB有效样本，转换暂存最多4000000字节，不能声称进程总内存64KiB。
3. 代码审查发现：消费端异常收尾曾在mutex外改atomic stop再notify，可能在生产者检查/睡眠之间漏唤醒。改为同mutex内改判定状态、锁外notify；wait带predicate检查stop。不把审查发现写成实际复现死锁。
4. 设备Pause/Padding调用移出共享mutex，避免GUI获取快照时等待驱动操作；源/设备各自由自己的线程拥有。
5. 消费异常也须停止并唤醒生产端，再join；生产端真实错误保留Failed。不detach、不超时后冒充退出。设备消费10秒无进展会失败，但不是任意驱动/I/O的硬停止保证。

## 构建与最终自动验证

根MiniClipStudio.slnx使用VS2026 MSBuild，Debug/x64与Release/x64均实际生成成功。最后一次代码变更（唤醒协议）之后重新生成两版本，并运行最终S04测试；没有用正在构建时的旧二进制冒充验证。原功能回归在该唤醒修正前执行，修正只涉及新AudioPlayer收尾路径，未改旧功能算法。

| 项目 | Debug证据（workspace下） | Release证据（workspace下） |
|---|---|---|
| S04最终：PCM+真实设备+CLI取消 | `playback-check-1794571117704483900/report.json` | `playback-check-1794571461611733400/report.json` |
| 录音/转换数值与错误 | `audio-check-1794570811614329200/report.json` | `audio-check-1794570939760638100/report.json` |
| S01探测/生命周期 | `probe-check-1794570811607032900/report.json` | `probe-check-1794570939767727300/report.json` |
| S02像素/全段解码/错误 | `video-check-1794570811678190900/report.json` | `video-check-1794570939747074900/report.json` |
| S03内容/时间/裁剪/取消/发布 | `export-check-1794570811617792700/report.json` | `export-check-1794570939744700600/report.json` |

S04命令：

```text
python -X utf8 tests/verify_playback.py --exe AudioCapture/x64/Debug/AudioCapture.exe --lifecycle build/lifecycle/Debug/LifecycleTests.exe --with-device
python -X utf8 tests/verify_playback.py --exe AudioCapture/x64/Release/AudioCapture.exe --lifecycle build/lifecycle/Release/LifecycleTests.exe --with-device
```

已知PCM覆盖8/16/24位/float、不同采样率、单双声道、中文空格路径和短尾；实际输出float核对帧数/频率/声道/有限数值与源哈希。S03 C API生成AAC供planar/首轨检查。无音轨/损坏/非法用法/预停止均验证。可选真实设备分支实际执行，未跳过。

真实设备静音检查：暂停180ms期间消费/提交计数冻结，恢复推进；96000帧（2秒）全部提交并消费；队列峰值8/8；本机实际设备缓冲4800帧；该测试饥饿区间0。满队列暂停后停止、快速重启、错误后回收与重入保护通过。

单次停止到join：Debug 61.772ms，Release 77.236ms。CLI定向向自己启动的进程组发送Ctrl+Break，两版本均退出3，取消到进程退出约96.679/107.912ms。这里只是本轮短样本观察，不是P95、硬实时上限或Release必定更快的结论；未测端到端听觉延迟。

## 未完成的人工验证与功能边界

尝试使用computer-use技能及可用窗口工具，但初始化均报`failed to write kernel assets: 系统找不到指定的路径。 (os error 3)`，尚未控制或观察产品窗口。因此**按钮/文件对话框/播放中关窗未记为实际通过**，也未要求用户批准一个不存在的危险动作。程序化API/真实WASAPI静音验证可正常运行。

待用户在VS清空旧--export参数、Ctrl+F5打开窗口，试听自己的WAV和S03 MP4，检查速度/音调、左右、音量、暂停/恢复、停止/关窗、长时和设备断开/切换。静音设备检查不代替听感。

未实现视频显示、AV同步、seek、倍速、网络、输出设备选择/热重连、多声道、音轨时间戳空洞修复或无锁低延迟引擎。played是submitted-padding应用格式消费估算，IAudioClock与视频调度留S05。没有新增Linux/GPU/AI推理或CPU采样成果。

## 下一步与理解检查

先完成人工验收，再按最小步骤做S05无声视频显示/时间调度，然后与音频时钟同步、最后seek。岗位性能线保留真实素材Release采样和单变量实验，不因已实现双线程就盲目并发。

1. 为什么已解码96000帧不能报告已经播放2秒？
2. 队列满时停止消费者，为什么仍需唤醒生产者？
3. 为什么暂停保留缓冲，而自然结束要等缓冲全部消费？

参考答案在本地Q69—Q71，用户理解与练习状态待反馈。
