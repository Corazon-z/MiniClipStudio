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
