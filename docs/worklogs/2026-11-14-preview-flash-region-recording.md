# 2026-11-14：S05固定频率闪白修正与手动区域录屏

## 用户现象与本次范围

用户报告S05能播放，但整块画面按固定频率闪白，要求代码可读/易维护，并把问题写入开发记录；录屏选择手动屏幕区域。先阅读根AGENTS、README、绘图/窗口/时钟与S06处理代码和项目配置。沿VS/Win32/FFmpeg SDK，不迁移框架，不覆盖legacy。

## 闪白根因和修正

原WM_PAINT先FillRect整个客户区为浅色，再调用DrawPreviewPicture清黑、缩放图片；每帧将中间擦除状态暴露给窗口。WM_ERASEBKGND返回1只能避免默认擦除，不能阻止代码自己反复刷浅色。

新增BufferedVideoSurface：GUI独占内存DC/DIB，大小不变复用，先在内存完成黑边/像素，再一次BitBlt到窗口；创建失败/尺寸变化/析构恢复原选中对象后归还GDI。非法输入不覆盖当前显示。新增ClearAroundPreview，只画viewport之外的面板背景并恢复DC裁剪，S05与录制窗口复用。

S05只在更新区域可见视频时取图/绘制/ReportPainted，避免控件局部重绘触发重复缩放或虚报画面已绘；暂停按钮文案仅变化时更新。没有借此提高解码线程、修改源fps或夸大GPU加速，文件播放时钟与seek语义保留。

现有离屏像素测试扩展双缓冲的颜色/方向、非法源保留旧图、反复尺寸变化与仅清外围。它证明绘图输出与路径契约，不自动证明物理屏幕在任何负载下零闪烁或流畅；实际观感仍需用户使用新版本验收。

## 区域录屏实现

ScreenRegionSelector定义物理像素矩形，支持双向拖选、桌面内裁剪、偶数尺寸、取消和限制。选择/采集各自临时DPI感知，不改系统设置。CameraWindow增加来源、选区、说明，区域录像开始最小化，Ctrl+Shift+F9停止保存，回收后恢复；原摄像头/麦克风功能保留。

CameraCaptureOptions增加screenRegion；共享引擎对区域使用FFmpeg gdigrab/desktop及偏移/尺寸/fps，其他解码、预览、H.264 MP4、停止/失败发布复用。区域首版无声，麦克风/系统声音没有混入无效选项；S07只有此基础分支落地，不写成完整S07完成。

新增ScreenCaptureTests和verify_screen，真实测试只抓自有色块窗口，核区域偏移、实际BGRA、输出尺寸/编码与重解。源PTS接受合法Unix微秒后减origin，避免因大时间值误判；范围差值仍检查安全边界。

## 额外调试与构建约束

- 初次默认Debug链接LNK1104，检查到旧默认AudioCapture进程仍运行。没有终止用户进程，先在build/region-capture独立编译；用户随后确认关闭旧窗口/调试，再更新原默认输出。
- 第一次区域样本请求2秒，重解42帧/4.2秒。原因是通用fps探测额外预读，修正仅屏幕fps_probe_size=0，并在规格探测成功后启用NONBLOCK；保留真实参数检查，避免过早EAGAIN。
- 可见窗口工具初始化仍报“failed to write kernel assets…os error 3”。没有实际操作拖选或观察用户窗口的闪白消失，不把色块/GDI数值测试当作人工体验通过。

## 验证证据

最终Debug/Release根解决方案均实际生成成功，默认AudioCapture/x64/配置和build/lifecycle/配置输出已更新。日志位于workspace/s07-final-Debug-build.log与s07-final-Release-build.log。

初步独立Debug区域报告：workspace/screen-check-1794656570208943000/report.json，该报告在帧率探测修正前通过旧宽时长范围；4.2秒样本保留作问题证据，不用来证明后续时长修正。最终两配置区域样本均20帧/2秒；16组报告如下。

## 待验收和下一步

用户用新默认版本检查固定频率闪白、播放观感、暂停/seek/缩放/遮挡恢复；区域检查拖动取消、不同DPI/多屏、热键/最小化恢复及长录。软件双缓冲消除已知中间擦屏路径，但未测屏幕刷新时序或给出通用性能提升。

本地六册、词汇/源码卡/问答与开发复盘同步；不上传笔记或私人媒体。三题：抑制WM_ERASE为何仍会闪白？双缓冲与增加队列为何不同？区域坐标为何要物理像素和偶数宽高？答案见六册新增章节。


### 最终回归清单

总报告：`workspace/s07-regression-1794657047380074100/report.json`。每项退出0，EXE哈希在运行期间保持一致。

| 检查 | Debug报告（workspace下） | Release报告（workspace下） |
|---|---|---|
| audio | `audio-check-1794657047646224500/report.json` | `audio-check-1794657124296177700/report.json` |
| probe | `probe-check-1794657048627107900/report.json` | `probe-check-1794657125553818300/report.json` |
| video | `video-check-1794657049541713200/report.json` | `video-check-1794657126476241000/report.json` |
| export | `export-check-1794657050899103100/report.json` | `export-check-1794657127553432500/report.json` |
| playback | `playback-check-1794657053809535400/report.json` | `playback-check-1794657130255500200/report.json` |
| preview | `preview-check-1794657065366902900/report.json` | `preview-check-1794657141131210200/report.json` |
| camera | `camera-check-1794657119058613700/report.json` | `camera-check-1794657193710920900/report.json` |
| screen | `screen-check-1794657121107711900/report.json` | `screen-check-1794657195730054700/report.json` |

本次camera是文件/生命周期回归，未新增摄像头拍摄；screen是真实gdigrab抓取测试自有色块。窗口体验仍未验收。
