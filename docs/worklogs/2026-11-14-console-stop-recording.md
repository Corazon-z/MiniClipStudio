# 2026-11-14 窗口录屏Ctrl+C留下草稿

用户澄清“不能录制”为看错，未对此修改提示；实际问题是窗口模式Ctrl+C后留下.part。代码核对发现CLI的处理器只在RunCameraCommand内登记，无参数GUI及独立GUI未接控制台信号。旧默认Debug在独立控制台真实Ctrl+C后返回0xC000013A（3221225786），证据：workspace/console-stop-check-1794659563027387700/report.json。第一次测试曾误用WaitForInputIdle判断控制台子系统GUI就绪，现改为仅观察测试PID的窗口，未把该测试工具失败当产品故障。

修复：新增apps/desktop/ConsoleStopSignal.h，RAII登记/撤销，回调只写静态atomic，GUI用exchange取走后走WM_CLOSE。main统一管理主窗下的拍摄/预览；独立--camera/--preview自己消费。回调不碰FFmpeg，worker仍独占输入与编解码资源；已有CameraSession Stop/TryJoin和Mp4Output Finish/Publish负责安全收尾。收到关闭后立即return，避免继续访问已销毁窗口数据。媒体SDK和编码参数未变，无架构迁移。

默认Debug/Release生成成功（workspace/console-stop-Debug-build.log、workspace/console-stop-Release-build.log）。新测试给自己单独创建的控制台发C/Break，核对进程清单后才发事件；三种GUI入口退出0。编码第10帧触发真实事件，经同一请求模块合作停止，发布MP4后完整重解，无.part残留。入口与编码测试分开，尚未完成真实用户拖选录屏全流程人工操作。两配置摄像头/文件录制回归也通过；前次16组是历史证据，没有声称此次全部重跑。

首次并行构建出现MSB6003/DirectoryNotFoundException，AudioCapture的CL.write.1.tlog被解析到tests/integration/x64/Debug目录；改用/m:1、BuildInParallel=false串行生成，不修改SDK或工程输出配置。CameraWindow还加choosingOptions/OptionDialogScope：保存和选区对话框的嵌套消息循环期间延后销毁数据，关闭请求后不启动新采集；需要取消对话框后完成退出，此项人工交互待验收。

- [报告1](../../workspace/console-stop-check-1794660119884939100/report.json)
- [报告2](../../workspace/console-stop-check-1794660289385216700/report.json)
- [报告3](../../workspace/camera-check-1794660131244582900/report.json)
- [报告4](../../workspace/camera-check-1794660294285435000/report.json)

主文件：apps/desktop/main.cpp、CameraWindow.cpp、VideoPreviewWindow.cpp及新ConsoleStopSignal.h；测试：CameraTests.h/.cpp、LifecycleTests.cpp、verify_console_stop.py；项目清单登记新头文件。README/PROGRESS/ROADMAP、录屏文档和六册同步，本地笔记不上传。未进行新性能测量或GPU优化。

Windows处理器机制参考[Microsoft HandlerRoutine](https://learn.microsoft.com/en-us/windows/console/handlerroutine)，封装收尾参考[FFmpeg Muxing](https://www.ffmpeg.org/doxygen/trunk/group__lavf__encoding.html)。Ctrl+C/Ctrl+Break与关闭控制台、停止调试、结束进程不同，后者可能绕过正常析构和保存；本次不保证强制退出或断电可恢复，不删除旧草稿。

三道理解题及答案见第四册Q107—Q109：为什么CLI处理器不保护GUI？为什么系统回调不能直接Finish编码器？为什么改.part扩展名不能恢复索引？下一步人工验收新短录的Ctrl+C保存，再逐项补长录与崩溃恢复设计。
