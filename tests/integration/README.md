# 链路验证

路径：`tests/integration/`。整个仓库是产品，本目录是一个职责模块。

## 职责

媒体输入、处理、输出与项目保存恢复。

## 状态与实施规则

2026-11-11建立目录。说明文件用于保留目录和解释边界，不代表功能完成。实际代码与迁移状态以docs/MODULES.md和工作记录为准。

先完成正确、可验证功能，再根据测量优化；每个新函数解释输入/输出、错误、所有权和线程。不得把占位类当成实现；不为每个目录机械增加DLL或线程。

## 已落地文件（2026-11-11）

- `LifecycleTests.cpp`：不访问麦克风的状态/资源/线程验证。
- `LifecycleTests.vcxproj`：独立测试工程，复用生产代码，无第二main冲突。

这里只实现列出的基础能力；其余功能继续按阶段验收，不宣称完整商业质量。

## S01扩展（2026-11-12）

`LifecycleTests.exe --make-probe-fixture 新文件.avi` 使用FFmpeg C API生成一个视频流和两个音频流，并验证控制器的后台探测分支。常规生命周期测试增加“未知不能显示成0”和“开始前取消探测”的检查。

配套的 `tests/verify_probe.py` 检查产品CLI的媒体字段、中文路径、按内容识别、输入不变和失败退出码。Debug/x64与Release/x64均已运行通过；不访问麦克风，也不代替真实窗口文件对话框的人工验收。具体证据见 `docs/worklogs/2026-11-12-s01-media-probe.md`。

2026-11-12：VideoFixtures.h/.cpp通过FFmpeg C API生成带B帧延迟的8帧MPEG4；LifecycleTests增加视频预取消检查，verify_video.py验证帧数/PNG实际像素/错误清理，Debug和Release均通过。

2026-11-13：S03基础CLI已落地，实际文件/所有权/调用顺序见docs/TRANSCODING.md与docs/MODULES.md；验证和性能边界见worklogs/2026-11-13-s03-transcode.md、PERFORMANCE_WORKFLOW.md。目录中其他占位说明不代表实现。

## S04验证入口

PlaybackTests.h/.cpp加入测试工程。`--decode-playback INPUT OUTPUT.f32`供数值验证，`--test-playback-device INPUT`用真实默认设备静音播放并检查暂停/继续、尾部、停止/重启和失败。测试输入须至少2秒。产品不包含这些测试命令。

`python -X utf8 tests/verify_playback.py --exe AudioCapture/x64/Debug/AudioCapture.exe --lifecycle build/lifecycle/Debug/LifecycleTests.exe --with-device`生成独立样本并核对。去掉--with-device只验证解码，不能声称设备通过；GUI和听感另行验收。

## S06已实现范围（2026-11-14）

新增--test-camera-session SOURCE NEW.mp4，实际检查像素、预停止、回调失败、重启与线程回收。 具体使用、文件分工及验收边界见[摄像头说明](../../docs/CAMERA_CAPTURE.md)。

## 本次区域与重绘更新（2026-11-14）

新增--test-screen-region与--test-screen-capture NEW.mp4；双缓冲测试验证真实GDI像素和错误不擦前台。 使用和边界见[区域说明](../../docs/SCREEN_RECORDING.md)。
