// 这里实现S04播放的Windows设备输出，AudioPlayer::Run先创建对象初始化COM并Open默认端点，
// 随后按实际容量和padding调用Write、Start、Wait，将AudioDecode准备好的PCM交给WASAPI共享音频引擎。
// 暂停只停止消费并保留位置；最终结束或失败时析构才释放本会话。文件解码和线程队列仍在上层，不混入设备适配代码。
#include "WasapiOutput.h"
#include "../../../media/audio/AudioDecode.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#pragma comment(lib, "ole32.lib")

namespace miniclip {
namespace {
// 各WASAPI/COM调用用CheckWindows检查HRESULT，失败时连同操作阶段和错误码抛给AudioPlayer报告。
void CheckWindows(HRESULT result, const char* stage) {
    if (SUCCEEDED(result)) return;
    char text[240]{};
    sprintf_s(text, "%s: HRESULT=0x%08lX (check default output device / 检查默认扬声器或耳机)", stage, static_cast<unsigned long>(result));
    throw std::runtime_error(text);
}
}
// AudioPlayer::Run在消费线程构造输出对象时，调用CoInitializeEx初始化该线程的COM环境。
// 其他线程已初始化COM也不能代替这里；成功返回（含S_FALSE）都需要析构时匹配CoUninitialize。
WasapiOutput::WasapiOutput() {
    CheckWindows(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize playback COM");
    comInitialized_ = true;
}
// Run正常或异常退出时先停AudioClient，再释放RenderClient等COM接口、关闭事件，最后CoUninitialize。
// 必须先用完接口再退出它依赖的COM环境；析构不向外抛异常，正常暂停路径的返回值由Pause检查。
WasapiOutput::~WasapiOutput() {
    if (client_ && started_) client_->Stop(); // 析构不抛异常；正常路径Pause会检查返回值。
    clock_.Reset(); render_.Reset(); client_.Reset(); device_.Reset(); enumerator_.Reset();
    if (ready_) CloseHandle(ready_);
    if (comInitialized_) CoUninitialize();
}
// Run开始播放前调用Open：CoCreateInstance取得枚举器，GetDefaultAudioEndpoint找到默认播放端点，
// Activate取得IAudioClient，再以float48k/stereo共享格式Initialize，最后设置事件并读取实际容量/RenderClient。
// 这是应用提交格式，Windows共享引擎负责与设备混音格式之间的兼容转换，不假定物理声卡一定运行48k双声道。
void WasapiOutput::Open() {
    if (client_) throw std::logic_error("WASAPI output already open");
    CheckWindows(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(enumerator_.GetAddressOf())), "Create audio device enumerator");
    CheckWindows(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, device_.GetAddressOf()), "Find default playback device");
    CheckWindows(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client_.GetAddressOf())), "Activate WASAPI output");
    // 统一应用格式并允许系统转换，Initialize请求约100ms缓冲；失败明确报告，不换成假输出。
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = PlaybackChannels;
    format.nSamplesPerSec = PlaybackRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    CheckWindows(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000, 0, &format, nullptr), "Initialize shared playback (100ms request)");
    // 注册音频引擎通知事件后，读取实际帧容量；RenderClient随后负责成对提供/提交写入缓冲。
    ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready_) throw std::runtime_error("Create WASAPI event failed");
    CheckWindows(client_->SetEventHandle(ready_), "Set playback event");
    CheckWindows(client_->GetBufferSize(&capacity_), "Read playback buffer capacity");
    if (!capacity_ || capacity_ > PlaybackRate * 2) throw std::runtime_error("Unexpected playback device buffer capacity");
    CheckWindows(client_->GetService(IID_PPV_ARGS(render_.GetAddressOf())), "Get audio render client");
}
// Run每轮调用GetCurrentPadding读取设备尚未消费的帧数，并核对它不超过实际容量。
// 用总容量减去这个值才是可写空间，不能把已经提交但未消费的位置覆盖掉。
UINT32 WasapiOutput::Padding() const {
    UINT32 padding = 0;
    CheckWindows(client_->GetCurrentPadding(&padding), "Read queued device frames");
    if (padding > capacity_) throw std::runtime_error("Invalid device padding");
    return padding;
}
// Run已根据padding算好空位后，Write通过GetBuffer借设备缓冲，逐样本乘软件增益，再ReleaseBuffer提交。
// 非有限值归零、过载值钳到[-1,1]；这里只接收已经解码的PCM，不负责选文件或解码编码包。
void WasapiOutput::Write(const float* samples, UINT32 frames, float volume) {
    BYTE* bytes = nullptr;
    CheckWindows(render_->GetBuffer(frames, &bytes), "Acquire device buffer");
    auto target = reinterpret_cast<float*>(bytes);
    for (UINT32 i = 0; i < frames * PlaybackChannels; ++i) {
        const float sample = samples[i] * volume;
        // 非有限值不能送进混音引擎；增益最大1，仍钳住异常过载源的浮点样本。
        target[i] = !std::isfinite(sample) ? 0.0f : (sample < -1.0f ? -1.0f : sample > 1.0f ? 1.0f : sample);
    }
    CheckWindows(render_->ReleaseBuffer(frames, 0), "Submit playback samples");
}
// Run在首次提交样本或暂停恢复后启动AudioClient；已启动时不重复调用，让设备继续消费保留的缓冲。
void WasapiOutput::Start() {
    if (!started_) { CheckWindows(client_->Start(), "Start playback device"); started_ = true; }
}
// Run响应暂停或自然结束时调用IAudioClient::Stop，保留接口与缓冲；暂停恢复不会从头解码。
// AudioPlayer::Stop则是终止整次会话的请求，Run随后退出并析构本对象，二者不能混为一谈。
void WasapiOutput::Pause() {
    if (started_) { CheckWindows(client_->Stop(), "Pause playback device"); started_ = false; }
}
// 当前块已交完或设备没有空位时，Run在这里用WaitForSingleObject等事件，最多10ms返回一次。
// 事件或正常超时都让外层重新读取padding并检查控制请求，等待失败则抛异常；不能把这个10ms当作整次停止耗时。
void WasapiOutput::Wait() {
    const DWORD result = WaitForSingleObject(ready_, 10);
    if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) throw std::runtime_error("Wait for WASAPI event failed");
}
// S05在Open之后才申请时钟服务；先在局部完成GetService/GetFrequency，全部成功再存入本对象。
// 这样失败不会留下“已经有接口但没有有效频率”的半初始化状态，也不改变S04打开设备的步骤。
void WasapiOutput::EnableClock() {
    if (!client_) throw std::logic_error("Open WASAPI output before enabling its clock");
    if (clock_) return;
    Microsoft::WRL::ComPtr<IAudioClock> clock;
    CheckWindows(client_->GetService(IID_PPV_ARGS(clock.GetAddressOf())), "Get playback hardware clock");
    UINT64 frequency = 0;
    CheckWindows(clock->GetFrequency(&frequency), "Read playback clock frequency");
    if (!frequency) throw std::runtime_error("WASAPI playback clock reported zero frequency");
    clock_ = std::move(clock);
    clockFrequency_ = frequency;
}
// 音频线程把设备流位置转为秒，S05再加上共同媒体原点/跳转锚点，得到视频应跟随的媒体时钟。
// S_FALSE表示取样精度不足，只再读一次；若仍不可靠就明确失败，不能无限重试或伪装成精确时钟。
double WasapiOutput::ClockSeconds() {
    if (!clock_ || !clockFrequency_) throw std::logic_error("Enable WASAPI clock before reading it");
    UINT64 position = 0;
    HRESULT code = clock_->GetPosition(&position, nullptr);
    if (code == S_FALSE) code = clock_->GetPosition(&position, nullptr);
    CheckWindows(code, "Read playback hardware clock position");
    if (code != S_OK) throw std::runtime_error("WASAPI clock position remained imprecise after retry");
    return static_cast<double>(position) / static_cast<double>(clockFrequency_);
}
// S05以这一步建立seek屏障：先停止本线程的写入和消费，再Reset清缓冲与设备流位置。
// 普通暂停只需Pause并保留数据；只有跳转等明确丢弃旧内容的操作才应调用这里。
void WasapiOutput::ResetStopped() {
    if (!client_) throw std::logic_error("Open WASAPI output before resetting it");
    Pause();
    CheckWindows(client_->Reset(), "Reset stopped playback stream");
}
}
