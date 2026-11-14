# 2026-11-14：S05基础视频预览与音画同步

## 范围、设计与代码

按用户要求继续S05，先阅读根约束、路线、S02/S03解码与S04播放代码、GUI及项目配置。本机VS2026/C++20/FFmpeg9与Windows SDK接口已核实；不迁移Qt/CMake/SDK，不搬旧录音或S04框架。按规划目录新增独立会话和窗口，保留已有入口。

实现本地首视频/首音轨、1倍速音画调度、暂停/继续、音量、秒数seek、停止/重播、EOF后seek。两个独立输入/生产者提供定时BGRA和PCM，Run拥有WASAPI/主时钟，GUI只绘制当前代次邮箱。队列限3张画面/8块声音；重复拆封装是为了清楚所有权和避免单轨满队列挡住另一轨，性能成本后续测量。

新增PreviewTypes、PreviewInput、VideoPreview、AudioPreview、VideoPlayer、VideoSurface、VideoPreviewWindow；WasapiOutput按需增加EnableClock/ClockSeconds/ResetStopped。主窗增加打开视频，CLI增加--preview，产品/测试工程与filters登记新文件。测试新增PreviewFixtures、PreviewClockTests、VideoSurfaceTests、VideoPlaybackTests、verify_preview.py。

每个新模块/函数用连贯中文交代调用者、处理步骤和资源/线程约束。数据在同一origin后换成微秒或48k音频帧；视频保留源PTS/VFR和B帧尾，seek用前置关键帧解码/flush、重建swr和请求代次拒绝旧结果。GUI消息不携带裸帧指针。原S00—S04接口保持，legacy未修改。

## 实际调试与修正

- 初次测试调用发生在测试工程仍链接时，运行到旧程序并显示旧用法；该次不算S05证据。之后确认构建真实退出才执行新测试。
- 初测padding已空，硬件时钟仍差约24ms：原重缓冲分支把时间跳到提交末尾，可能过早结束。修正为硬件时钟也覆盖已提交内容才Reset；自然EOF继续等硬件尾部，最终位置更新到4秒。
- NUT夹具元数据估计4.46秒，实际归一解码尾4秒。没有通用减origin或按估计值截断，改以实际解码末尾定最终时长；尾静音只补到已准备的视频覆盖进度，防多补设备一整缓冲。
- 视频先结束时保持末图；跳到音频尾段也重新解本代末图，finalFrame区分这种有意保持与迟到。无声视频改用可暂停steady_clock，声音先结束分块补静音。
- Python参考值ceil(1.1*48000)因浮点误差误成52801；改为整数微秒有理换算，实际PCM首样本52800、剩139200本来正确。
- 交叉检查收口Stop后Start、EOF完成状态复制、最小化零面积绘制统计、单帧通知合并和代次屏障。设备API不持共享锁，异常也唤醒并真实join生产者。

## 构建与验证证据

根方案Debug/x64和Release/x64都实际生成成功，产品与LifecycleTests均已更新；没有用旧二进制冒充结果。所有已有脚本执行退出0。

| 验证 | Debug报告（workspace下） | Release报告（workspace下） |
|---|---|---|
| S05完整数值/GDI/真实设备/seek/时钟 | `preview-check-1794641856418066700/report.json` | `preview-check-1794642042720213400/report.json` |
| 音频转换 | `audio-check-1794642042481230600/report.json` | `audio-check-1794642042611587500/report.json` |
| S01探测/生命周期 | `probe-check-1794642042497501400/report.json` | `probe-check-1794642042616826000/report.json` |
| S02解码/像素/排空 | `video-check-1794642042503823900/report.json` | `video-check-1794642042658312900/report.json` |
| S03转码/时间/裁剪/取消 | `export-check-1794642042569294400/report.json` | `export-check-1794642042655768500/report.json` |
| S04真实设备播放回归 | `playback-check-1794642205174155700/report.json` | playback-check-1794642292273614800/report.json |

S05已实际验证：4个GOP、B帧、100帧、VFR交替20/60ms、音轨偏移、音轨先结束、无音轨，实际PTS/灰度/PCM频率/采样数；1.10秒seek得到1.08—1.12秒画面，跨GOP/暂停seek/快速替换/打开中停止/EOF后seek/回调失败回收。GDI读取CreateDIBSection真实像素核对BGRA、方向、黑边、清屏和边界。

真实WASAPI检查GetPosition/GetFrequency换算、推进/暂停冻结/Reset归零，非假时钟。完整4秒声音提交192000帧；延迟0.2秒时插9600静音，媒体尾4.2秒；声音2秒/视频4秒时补96000静音；10秒源提交480000帧并自然结束。两构建对应案例队列峰值不超3/8，本轮非末图的调度/邮箱迟到与饥饿计数均0。仅代表本机这些合成短负载，不是通用零误差、P95/P99或性能提速结论。

## 仍需人工验收与明确限制

可见窗口工具再次初始化失败：`failed to write kernel assets: 系统找不到指定的路径。 (os error 3)`。因此没有声称实际控制过预览窗口；离屏GDI、邮箱调度、WM_PAINT统计和物理屏幕/音箱呈现分开。请用户在VS用真实素材验收打开、缩放/遮挡恢复、音量、暂停seek、播放中关闭及听感；长时稳定性/断连未验收。

当前基础1倍速，不含倍速/保调、HDR/旋转/SAR、字幕、网络、设备热切换、GPU解码。源格式/时间戳有明确范围与错误；不是全格式商业播放器。倍速按路线作为后续独立小步。

README、路线/模块/进度、VIDEO_PREVIEW和S01—S04串讲续篇更新；本地F21/运行22、两份主笔记、S05专篇、教程/VS/目录/图解、Q85—Q90与面试内容同步；75份逐行副本和78文件指纹刷新。笔记仍Git忽略，不暂存/上传。

下一步先人工验收，再继续倍速/兼容或S06具体小步。三题：为什么共用origin？seek为什么不止flush？padding0为什么还要等硬件时钟？答案见本地Q85—Q87，练习未执行不标掌握。
