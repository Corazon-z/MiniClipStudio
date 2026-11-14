// WasapiOutput是项目给S04编写的Windows输出适配器，AudioPlayer::Run用它把已解码PCM交给默认扬声器/耳机。
// 真正与Windows音频引擎沟通的是WASAPI接口；本类负责初始化、查询缓冲空位、提交样本和最后归还资源。
// 所有方法都在创建它的消费线程调用，GUI/解码线程只向AudioPlayer发请求，不能直接操作这个设备会话。
// S05按需启用IAudioClock读取设备时钟，跳转时清掉旧缓冲；S04原接口仍只按容量和padding播放。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
// Windows SDK的AudioClient/AudioRenderClient管理音频会话与GetBuffer/ReleaseBuffer，不属于FFmpeg。
#include <audioclient.h>
// MMDevice接口查找当前默认输出端点，WRL的ComPtr负责自动释放COM接口引用。
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <cstdint>

namespace miniclip {
class WasapiOutput {
public:
    // Run创建输出对象时先初始化当前线程的COM环境，后面的Windows声音接口才能在本线程使用。
    WasapiOutput();
    // Run退出时先停止并释放声音接口和事件，最后结束本线程的COM环境，异常离开也会执行。
    ~WasapiOutput();
    // 输出会话与事件只归一个对象管理，不能复制后重复释放。
    WasapiOutput(const WasapiOutput&) = delete;
    // 赋值同样不能复制设备资源的管理关系。
    WasapiOutput& operator=(const WasapiOutput&) = delete;
    // Run在启动生产线程前打开默认eRender/eConsole输出端点，以共享模式初始化，再取得事件、实际容量和RenderClient。
    // 这是播放端点，与录音界面的麦克风选择无关；共享模式允许Windows同时混合其他应用的声音。
    void Open();
    // Run用padding查询已提交但尚未消费的帧数，再从总容量扣掉它得到可写空间。
    UINT32 Padding() const;
    // 返回Open从设备读到的实际帧容量，请求100ms不保证Windows正好分配100ms。
    UINT32 Capacity() const { return capacity_; }
    // Run确认设备有足够空位后，用Write借缓冲、乘软件音量并ReleaseBuffer提交，过滤异常浮点值和过载。
    // 只改变本程序新提交的样本，不能追溯调整已交给设备的缓冲，也不改系统总音量。
    void Write(const float* samples, UINT32 frames, float volume);
    // Run首次送入样本或继续播放时启动AudioClient，让Windows开始消费已经提交的声音。
    void Start();
    // Run暂停时用IAudioClient::Stop停止消费并保留缓冲，继续时可接着播放。
    // 它不同于AudioPlayer::Stop：后者终止整次播放并最终释放设备会话。
    void Pause();
    // Run等音频事件时最多休眠10ms再返回，避免空转，也让外层定期检查停止/暂停；这不是完整停止延迟保证。
    void Wait();
    // S05输出线程在Open之后按需取得IAudioClock及其计数频率；S04不调用，因此不会新增必需服务。
    void EnableClock();
    // 将设备位置除以GetFrequency返回的频率得到本次设备流的秒数，不能假定位置本身就是48k样本编号。
    // 只在输出线程调用，S05再把这个设备相对时间映射到当前代次的媒体时间。
    double ClockSeconds();
    // S05跳转前先暂停再Reset，丢弃待播声音并将设备流时钟归零；调用时不能有未ReleaseBuffer的借用。
    void ResetStopped();
private:
    // COM环境必须晚于全部接口释放；析构中明确reset，再CoUninitialize。
    bool comInitialized_ = false;
    bool started_ = false;
    HANDLE ready_ = nullptr;
    UINT32 capacity_ = 0;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioRenderClient> render_;
    Microsoft::WRL::ComPtr<IAudioClock> clock_;
    uint64_t clockFrequency_ = 0;
};
}
