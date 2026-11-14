# 2026-11-14：S06基础摄像头预览与音视频录制

## 范围与设计

用户要求继续S06并维护全部相关文档。已阅读AGENTS、README、Task00历史约束、当前路线、S03/S05源码、设备枚举与项目配置。沿既有VS2026/v145/x64/C++20和当前FFmpeg SDK新增CameraCatalog、CameraCapture、CameraSession、CameraWindow、CameraCommands；不迁移框架，不覆盖legacy，不更换SDK。

实现设备/实际模式、连续预览、无声或麦克风H.264/AAC MP4、模式选择、输出CFR选择、倒计时、停止保存、异步窗口回收及CLI诊断。摄像头/麦克风使用同一dshow输入，保留公共时间；本阶段无录制暂停/无缝预览转录制/设备热恢复/GPU。

CameraCatalog用原生DirectShow/COM独立枚举视频类别与能力，避免联合枚举中音频失败连带遮蔽摄像头；实际媒体采集仍用FFmpeg C API。CameraCapture复用TranscodeTrack/Mp4Output，给轨道增加默认空的同步视频观察者；S03不传回调，原接口调用保持有效。轨道新增常见8位NV12/YUYV422/UYVY422/YUV422P/YUVJ422P/BGRA/BGR0输入，S02/S05范围不因此自动扩大。

## 修改与所有权

新增五组产品h/cpp及CameraTests.h/.cpp、verify_camera.py；入口分发/主窗口互斥与关闭、两个vcxproj和产品filters登记对应文件。GUI只拥有会话/自有图，worker拥有FFmpeg资源。预取最多512包/32MiB，输入缓冲请求16MiB，显示邮箱一张。停止请求仅结束采集，轨道继续排空，错误则清草稿，完成才发布。

## 实际调试

1. 首次编译缺Windows声明，显式补头；默认沙箱构建在报错后未退出，已中止该会话，之后沙箱外单节点/nr:false构建，确认实际退出再跑测试。
2. 首轮真实MJPEG/YUYV模式都协商失败。交叉比较COM与FFmpeg能力日志后，发现帧率有理数近似/100ns取整边界；CameraRate选择间隔中点且检验往返。修正后两类格式的实际采集路径可进入处理；最终硬件回归固定使用MJPEG模式4。
3. 数值测试曾把PCM输出文件误当作带自动.f32后缀的前缀，改为读取实际输出路径；这次失败不计为通过。
4. 沙箱内设备枚举返回限制性错误；最终设备回归在正常设备环境执行。摄像头枚举独立于麦克风，GUI麦克风枚举失败可警告并继续无声使用。
5. Poll的初始快照可能早于线程最终状态，join后重新读取，避免漏填设备；最小化零面积不报告绘制失败。

## 构建与运行证据

实际Debug/x64与Release/x64产品、LifecycleTests生成成功。最终日志为本机workspace/s06-debug-verified.log与s06-release-verified.log。顺序运行S00—S06七组脚本×两配置，14组均退出0。

总报告：本机 `workspace/s06-regression-1794648815897950200/report.json`。目录被Git忽略，不将私人拍摄内容上传。

| 阶段 | Debug报告目录（workspace下） | Release报告目录（workspace下） |
|---|---|---|
| S00 | audio-check-1794648816046855800 | audio-check-1794648901924184800 |
| S01 | probe-check-1794648816838523900 | probe-check-1794648903076310200 |
| S02 | video-check-1794648817550435300 | video-check-1794648904133232400 |
| S03 | export-check-1794648818867892800 | export-check-1794648905258916800 |
| S04真实音频输出 | playback-check-1794648821589293800 | playback-check-1794648908037296300 |
| S05像素/解码/真实时钟/seek | preview-check-1794648833201776400 | preview-check-1794648918954955200 |
| S06含真实摄像头 | camera-check-1794648886859362500 | camera-check-1794648972504435900 |

S06数值核对：4秒100帧B帧源全解码，真实BGRA/灰度/alpha、H.264/AAC成片、0.2秒音轨偏移及前导PCM静音；无声、1.2秒30帧裁剪、停止后前缀成片、预停止、倒计时停止、回调失败、后台重启、缺失输入/坏选项、已有输出与源哈希保护、临时文件清理。

本机HP摄像头与麦克风，读到8个模式；640×360 MJPEG短录最终重解：Debug无声120帧/4.0秒，有声132帧/视频4.4秒/音频4.35秒；Release无声121帧/4.033333秒，有声129帧/视频4.3秒/音频4.2秒。真实seconds=4限制主循环墙钟，探测预读/起点/尾部排空让成片可略长，不能称精确4秒裁剪。

端点差50—100ms是这些短录的包尾观测，不等于物理口型误差；当前不强制两轨严格齐尾，也没有完成长期漂移校正。没有CPU采样、GPU或性能提速结论。输入/成片帧数不同可能来自CFR重排，不直接归因为记录丢帧。

## 仍待人工验收

补充专项：原生枚举改为COM后，代码复核发现`--camera-options`独立进程也要自行注册FFmpeg dshow；已修正并分别编译/运行两配置诊断命令。verify_camera新增该项。追加Release测试曾先于链接结束启动、命中旧程序，失败不计通过；确认生成结束后重跑有效。最终Debug `camera-check-1794649633373317700/report.json`、Release `camera-check-1794649684310103800/report.json` 各27步通过（workspace下），均含真实设备。S00—S05未再改代码，主回归证据不变。

另有Release原始YUYV422模式7实际2秒短录：60个成片帧、视频尾2.0秒、无音轨，重解成功；本机产物workspace/s06-real-release-yuyv.mp4，日志s06-real-release-yuyv.log。不同驱动的同名格式仍需单独验证。

使用computer-use技能尝试窗口验收，初始化再次报“failed to write kernel assets: 系统找不到指定的路径。(os error 3)”。没有实际操作可见窗口，不把GDI离屏或CLI摄像头短录冒充窗口通过。需用户检查窗口模式选择、缩放/最小化、倒计时、停止/关闭、回放听感、设备占用/隐私拒绝/拔出与更长录制。不能承诺不响应的驱动硬超时退出。

## 文档与下一步

新增CAMERA_CAPTURE，更新README、路线/模块/进度、相关功能用法、目录README与测试说明；六册完整补充S06的流程、词汇/API、源码实现、理解题/面试、设计复盘与图解。旧学习文档补当前入口，历史worklog/归档不改写当时结果，源码阅读副本/指纹刷新；local-notes不暂存或上传。

先做人工S06验收和较长素材/录制测量，再决定齐尾/漂移或性能小实验，不直接开始S07。三题：停止与取消为何不同？单图邮箱覆盖为何不直接代表成片丢帧？共同origin保留什么？答案见CAMERA_CAPTURE与六册。
