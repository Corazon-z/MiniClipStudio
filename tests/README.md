# tests

产品职责分组目录，具体模块边界和已实现状态见根目录docs/MODULES.md。子目录说明不是功能实现；不要求每目录对应库、类或线程。

## S06已实现范围（2026-11-14）

verify_camera.py和integration/CameraTests覆盖S06，--with-camera才实际拍摄；私人样本和报告在忽略的workspace。 具体使用、文件分工及验收边界见[摄像头说明](../docs/CAMERA_CAPTURE.md)。

## 本次区域与重绘更新（2026-11-14）

新增verify_screen.py，--with-desktop仅捕获测试自己的色块窗口，核坐标/像素/MP4；默认不录私人桌面。 使用和边界见[区域说明](../docs/SCREEN_RECORDING.md)。

## 窗口Ctrl+C退出与录制收尾

运行`python tests/verify_console_stop.py --exe AudioCapture/x64/Debug/AudioCapture.exe --lifecycle build/lifecycle/Debug/LifecycleTests.exe`。脚本给自己创建的独立控制台发真实C/Break事件，验证三种GUI入口正常退出和编码中断后MP4重解、无草稿遗留；不会向用户控制台广播。界面入口与编码收尾分开验证，用户完整录屏操作仍需人工验收。
