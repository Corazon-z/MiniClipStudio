# Windows输出后端

路径：`src/platform/windows/output/`。整个仓库是产品，本目录是一个职责模块。

## 职责

音频设备播放、图形呈现与系统句柄。

## 状态与实施规则

2026-11-11建立目录。说明文件用于保留目录和解释边界，不代表功能完成。实际代码与迁移状态以docs/MODULES.md和工作记录为准。

先完成正确、可验证功能，再根据测量优化；每个新函数解释输入/输出、错误、所有权和线程。不得把占位类当成实现；不为每个目录机械增加DLL或线程。

## S04已实现

WasapiOutput.h/.cpp实现Windows默认播放设备共享输出，固定应用格式float48k/stereo、系统适配混音格式。ComPtr/事件/COM均在播放消费线程创建使用释放；支持容量/padding、写入增益、暂停/启动和事件等待。视频呈现仍未实现。详见docs/AUDIO_PLAYBACK.md。

## 本次区域与重绘更新（2026-11-14）

BufferedVideoSurface拥有可复用DC/DIB，ClearAroundPreview避免浅色擦视频；不是硬件/GPU呈现。 使用和边界见[区域说明](../../../../docs/SCREEN_RECORDING.md)。
