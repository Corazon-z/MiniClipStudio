# Windows捕获后端

路径：`src/platform/windows/capture/`。整个仓库是产品，本目录是一个职责模块。

## 职责

设备、摄像头、屏幕、系统声音适配；具体后端先实验。

## 状态与实施规则

2026-11-11建立目录。说明文件用于保留目录和解释边界，不代表功能完成。实际代码与迁移状态以docs/MODULES.md和工作记录为准。

先完成正确、可验证功能，再根据测量优化；每个新函数解释输入/输出、错误、所有权和线程。不得把占位类当成实现；不为每个目录机械增加DLL或线程。

## S06已实现范围（2026-11-14）

CameraCatalog.h/.cpp已实现独立视频类别和IAMStreamConfig模式查询；核心采集仍通过FFmpeg。 具体使用、文件分工及验收边界见[摄像头说明](../../../../docs/CAMERA_CAPTURE.md)。

## 本次区域与重绘更新（2026-11-14）

ScreenRegionSelector处理物理坐标、临时DPI和规范化，gdigrab输入由共享媒体引擎打开。 使用和边界见[区域说明](../../../../docs/SCREEN_RECORDING.md)。
