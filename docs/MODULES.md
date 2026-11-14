# 产品模块目录与状态

2026-11-11。所有目录已落地，目录说明不等于功能实现。现有录音基础提取已完成；剩余目录保留职责说明，不生成空壳实现。

| 路径 | 职责 |
|---|---|
| `apps/desktop/` | 组装Win32窗口与应用控制，不执行长时间媒体处理 |
| `apps/cli/` | 参数解析、实验与自动验证，复用生产媒体链路 |
| `src/common/` | 错误类型、时间值、日志契约；不依赖GUI |
| `src/platform/windows/capture/` | 设备、摄像头、屏幕、系统声音适配；具体后端先实验 |
| `src/platform/windows/output/` | 音频设备播放、图形呈现与系统句柄 |
| `src/media/ffmpeg/` | 库资源、错误和编码边界；不拥有窗口 |
| `src/media/probe/` | 文件/流编码、时间、格式和能力信息 |
| `src/media/demux/` | 打开、读取、seek、流选择与取消 |
| `src/media/decode/` | 编码包到音视频帧，正确处理EAGAIN与排空 |
| `src/media/audio/` | 重采样、样本格式、声道、音量、混音 |
| `src/media/video/` | 尺寸、像素格式、颜色空间/范围转换 |
| `src/media/encode/` | 帧到目标编码包；能力查询与参数检查 |
| `src/media/mux/` | 文件头、时间戳、写包、收尾和输出发布 |
| `src/capture/` | 设备目录及多源捕获协作，不操作UI |
| `src/capture/audio/` | 当前音频录制生命周期、输入与输出链路 |
| `src/capture/camera/` | 视频拍摄、配置、时间戳与录音同步 |
| `src/capture/screen/` | 窗口/桌面捕获与声音组合 |
| `src/capture/session/` | 多个源的开始/停止、共享时钟和异常策略 |
| `src/application/` | 录制/导入/编辑/导出任务和生命周期 |
| `src/ui/` | 素材、时间轴、录制、预览、导出面板 |
| `src/assets/` | 稳定ID、元数据、缩略图、波形、代理与重定位 |
| `src/project/` | 项目、序列、轨道、片段，数据不含活动媒体指针 |
| `src/timeline/` | 裁剪/分割/速度/时间映射与轨道规则 |
| `src/effects/audio/` | 增益、淡入淡出和后续音频效果 |
| `src/effects/video/` | 画面处理、标题、转场和关键帧 |
| `src/render/` | 按项目时间生成合成画面与混音；预览导出共用语义 |
| `src/playback/` | 缓冲、时钟、seek、暂停与实时输出 |
| `src/export/` | 预设、进度、取消和完整文件发布 |
| `src/persistence/` | 项目版本、原子保存、自动保存与迁移 |
| `src/ai/runtime/` | 推理引擎、模型版本、取消、资源约束；暂不选择具体引擎 |
| `src/ai/vision/` | 检测、关键点、分割与跟踪，输出结构化结果 |
| `src/ai/effects/hairstyle/` | 头发区域与时序一致性处理，待模型/质量验证 |
| `src/ai/effects/face/` | 授权素材上的面部变换与时序融合，不把FFmpeg误当AI模型 |
| `src/ai/policies/` | 授权/用途/模型许可记录、结果标识和用户数据处理契约 |
| `resources/icons/` | 可分发产品图标与来源记录 |
| `resources/presets/` | 版本化的录制、效果与导出配置，不写死硬件支持 |
| `resources/models/` | 仅保存模型版本/校验/许可说明，不提交大体积权重或私人素材 |
| `build-config/vs/` | 共享属性表、SDK配置说明和平台策略 |
| `packaging/` | 安装、运行时依赖、升级与回滚说明 |
| `packaging/licenses/` | 许可证、第三方声明和实际构建信息 |
| `third_party/` | 依赖来源/版本/构建配置与分发条件 |
| `tests/unit/` | 纯逻辑、状态/时间映射/项目模型 |
| `tests/integration/` | 媒体输入、处理、输出与项目保存恢复 |
| `tests/manual/` | 真实设备、GUI、试听、断开、磁盘与长时步骤 |
| `tests/fixtures/` | 可分发的小样本或生成说明，媒体忽略规则需显式审核 |
| `tests/performance/` | 先留测量规范，不提前优化 |
| `docs/decisions/` | 背景、候选、选择、代价与验收证据 |
| `docs/specifications/` | 时间、格式、状态、序列化与错误语义 |
| `docs/worklogs/` | 逐步设计、实现、问题和实际测试记录 |
| `tools/` | 构建/诊断/测试辅助，不替代FFmpeg C API |

## 依赖方向

UI/apps → application → capture / playback / export；这些服务使用media与platform。project/timeline不依赖UI。render共享时间轴和效果语义。AI效果通过runtime/vision取得帧处理结果，再交render，不直接写容器或修改窗口。

## 文件说明规则

每个README解释模块；真实.h/.cpp顶部写用途、调用线程、资源所有者与限制。暂不为未实施功能生成空.h/.cpp。legacy是原实验，AudioCapture是兼容VS构建目录，不是产品边界。

## 未来接口与状态

公共数据先保持简单值类型；AVFrame/AVPacket仅在媒体内部按明确所有权流转。项目持久化不保存FFmpeg/窗口指针。先完成本地录音正确性，再做摄像头与单素材处理，播放器/时间轴/多轨和AI依次验收。

## 当前实现文件表

| 文件 | 功能 |
|---|---|
| `apps/desktop/main.cpp` | Win32窗口创建/按钮/定时器/关闭请求；媒体和线程交src处理 |
| `apps/cli/AudioCommands.h` | 无GUI命令入口声明 |
| `apps/cli/AudioCommands.cpp` | 参数解析、转换/录音/取消/合成实验，调用同一生产链路 |
| `src/application/RecordingController.h` | Job数据及控制器接口，GUI唯一管理者 |
| `src/application/RecordingController.cpp` | 启动/零等待join/回收/退出，拒绝重入，禁止复制 |
| `src/capture/DeviceCatalog.h` | 自有UTF-8设备数据和枚举接口 |
| `src/capture/DeviceCatalog.cpp` | FFmpeg清单过滤复制与错误清理 |
| `src/capture/audio/AudioRecorder.h` | 会话参数/结果与worker调用契约 |
| `src/capture/audio/AudioRecorder.cpp` | 输入/音频流/解码/取消/收尾编排与合成实验 |
| `src/media/ffmpeg/FfmpegSupport.h` | 资源类型/删除器/不可复制InputResources及工具声明 |
| `src/media/ffmpeg/FfmpegSupport.cpp` | UTF-8路径、错误检查、帧包分配 |
| `src/media/mux/WavWriter.h` | 输出资源所有者、调用顺序状态和公开契约 |
| `src/media/mux/WavWriter.cpp` | 重采样、PCM编码、时间戳、WAV收尾、文件发布 |
| `build-config/vs/FFmpeg.props` | 共享SDK根目录，验证头路径，允许本机覆盖 |
| `build-config/vs/FFmpeg.local.props.example` | 本机配置模板；复制出的.local.props被Git忽略 |
| `tests/integration/LifecycleTests.cpp` | 不访问麦克风的状态/资源/线程验证 |
| `tests/integration/LifecycleTests.vcxproj` | 独立测试工程，复用生产代码，无第二main冲突 |

## 2026-11-12 S01

MediaProbe.h/.cpp已实现本地容器及逐流参数探测、未知字段和报告排版。CLI --probe与GUI后台共用，具体说明见MEDIA_PROBE.md。该功能不做完整媒体解码/播放，S02尚未实施。

## S02新增实际文件（2026-11-12）

- src/media/video/VideoFrameExport.h/.cpp：同步解码、首帧RGB/PNG、全段计数与可靠发布，不是播放器或整视频转码。
- tests/integration/VideoFixtures.h/.cpp：测试专用MPEG4延迟输入；tests/verify_video.py：真实PNG像素、帧数、错误与清理验证。
- 产品/测试vcxproj登记源码，测试链接swscale；无GUI按钮变更。使用与所有权见VIDEO_FRAME_EXPORT.md。

## S03实际文件（2026-11-13）

- src/export/Transcode.h/.cpp与TranscodeTiming.h：同步导出编排、自有参数/进度/指标、计时。
- src/media/encode/TranscodeTrack.h/.cpp：每轨解码/过滤/编码、时间基与排空；src/media/mux/Mp4Output.h/.cpp：独占文件与AVIO、写尾及可靠发布。
- apps/cli/TranscodeCommands.h/.cpp：--export、参数、Ctrl+C与报告；现有录音/探测UI不迁移。
- tests/integration/TranscodeTests.h/.cpp、tests/verify_export.py、tools/benchmark_export.py：已知输入、独立输出重读、取消/错误与重复测量。
- 测试工程新增avfilter链接；产品本来已链接avfilter。所有新头/cpp加入VS项目/筛选器（测试头加入测试工程）。范围见TRANSCODING.md，不等于S04播放器、GPU或云服务完成。

## S04实际文件（2026-11-13）

- src/media/audio/AudioDecode.h/.cpp：本地第一音轨解码、真实格式检查、float48k双声道转换、1024帧块与排空。
- src/platform/windows/output/WasapiOutput.h/.cpp：消费线程内的COM、默认WASAPI共享输出、padding/空闲容量、增益、事件与RAII。
- src/playback/AudioPlayer.h/.cpp：两线程会话、8块有限队列、互斥/条件变量、暂停/继续/停止与退出回收。
- apps/cli/PlaybackCommands.h/.cpp：--play、音量、P/Q与Ctrl+C；apps/desktop/main.cpp新增播放控件及异步关窗。
- tests/integration/PlaybackTests.h/.cpp与tests/verify_playback.py：PCM数值和真实输出设备静音验证。产品/测试工程登记源码，ole32通过pragma链接，无SDK/Qt/构建迁移。
- 运行与逐函数职责见[AUDIO_PLAYBACK.md](AUDIO_PLAYBACK.md)。录音和播放的线程控制不同，暂保留两套清晰的小会话，不为了统一名称做大重构。

## S05实际文件（2026-11-14）

PreviewTypes定义带时间/代次的数据；PreviewInput封装每线程独立输入与seek；VideoPreview/AudioPreview分别解码BGRA和定时PCM；VideoPlayer协调3条后台执行路线、3帧/8块队列、主时钟和邮箱。VideoSurface用GDI呈现，VideoPreviewWindow接窗口交互。WasapiOutput增量提供Clock/Reset，S04路径保留。

测试新增PreviewFixtures、PreviewClockTests、VideoSurfaceTests、VideoPlaybackTests与verify_preview.py。具体函数/所有权和调用图见[视频预览](VIDEO_PREVIEW.md)，不把下一步倍速、硬件解码、设备热恢复写成已完成。

## S06实际模块（2026-11-14）

CameraCatalog负责DirectShow设备/实际模式；CameraCapture同步组织同图采集、公共PTS与可选MP4；CameraSession拥有后台任务和单图邮箱；CameraWindow与CameraCommands提供窗口/CLI。复用TranscodeTrack/Mp4Output/VideoSurface，观察者默认空，旧S03调用保留。

新增src/capture/camera、session及platform/windows/capture真实代码，不再是纯README规划。屏幕、工程模型、时间轴与AI仍未实现。资源/函数/线程图和测试见[摄像头说明](CAMERA_CAPTURE.md)。

## 本次显示与区域模块（2026-11-14）

VideoSurface新增BufferedVideoSurface/ClearAroundPreview，窗口拥有后台DC/DIB；S05与录制窗口复用。ScreenRegionSelector负责物理选区/规范化/DPI，CameraCapture的screenRegion分支使用gdigrab；入口与会话继续复用，不增加空壳录制类。ScreenCaptureTests/verify_screen验证纯坐标与自有窗口的真实捕获，S07仅基础无声区域分支。
