// S06窗口和CLI在后台查询这里；先枚举设备，再读驱动模式，实际采集仍由FFmpeg dshow完成。
#define NOMINMAX
#include "CameraCatalog.h"
#include "../../../media/ffmpeg/FfmpegSupport.h"
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>
#include <sstream>
#include <iomanip>
#include <locale>
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")
#pragma comment(lib, "Strmiids.lib")
namespace miniclip {
namespace {
using Microsoft::WRL::ComPtr;
// HRESULT与FFmpeg错误码不同；设备忙/权限问题保留具体操作和系统码。
void CheckCom(HRESULT code, const char* stage) {
    if (FAILED(code)) throw std::runtime_error(std::string(stage) + ": HRESULT=" + std::to_string(static_cast<unsigned long>(code)));
}
struct Apartment {
    // 这个短查询独占调用线程的COM初始化；接口在Apartment析构前已经释放。
    Apartment() { CheckCom(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "Initialize camera COM"); }
    ~Apartment() { CoUninitialize(); }
};
struct MediaType {
    AM_MEDIA_TYPE* value = nullptr;
    // GetStreamCaps连同格式块一起分配，必须释放内部块、接口引用和外层结构。
    ~MediaType() {
        if (!value) return;
        CoTaskMemFree(value->pbFormat);
        if (value->pUnk) value->pUnk->Release();
        CoTaskMemFree(value);
    }
};
// 支持常见软件8位摄像头格式；未知子类型仍列出，但不能直接选择录制。
void DescribeFormat(const GUID& subtype, CameraMode& mode) {
    const DWORD tag = subtype.Data1;
    if (subtype == MEDIASUBTYPE_RGB24) mode.pixelFormat = "bgr24";
    else if (subtype == MEDIASUBTYPE_RGB32) mode.pixelFormat = "bgr0";
    else if (tag == MAKEFOURCC('Y','U','Y','2')) mode.pixelFormat = "yuyv422";
    else if (tag == MAKEFOURCC('U','Y','V','Y')) mode.pixelFormat = "uyvy422";
    else if (tag == MAKEFOURCC('N','V','1','2')) mode.pixelFormat = "nv12";
    else if (tag == MAKEFOURCC('I','4','2','0') || tag == MAKEFOURCC('I','Y','U','V')) mode.pixelFormat = "yuv420p";
    else if (tag == MAKEFOURCC('M','J','P','G')) mode.codec = "mjpeg";
    else if (tag == MAKEFOURCC('H','2','6','4')) mode.codec = "h264";
    mode.supported = !mode.pixelFormat.empty() || !mode.codec.empty();
    if (!mode.pixelFormat.empty()) mode.codec = "rawvideo";
}
}
// 直接枚举视频类别，避免FFmpeg联合清单被“机器没有音频输入”的错误连带清空。
// 唯一名使用与dshow一致的moniker转义；名字和描述复制后才释放COM对象。
std::vector<CameraDevice> ListCameraDevices() {
    Apartment apartment;
    ComPtr<ICreateDevEnum> devices;
    CheckCom(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices)), "Create camera enumerator");
    ComPtr<IEnumMoniker> list;
    const HRESULT status = devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &list, 0);
    CheckCom(status, "Enumerate cameras");
    if (status == S_FALSE || !list) return {};
    std::vector<CameraDevice> result;
    ComPtr<IMoniker> moniker;
    HRESULT next;
    while ((next = list->Next(1, moniker.ReleaseAndGetAddressOf(), nullptr)) == S_OK) {
        LPOLESTR display = nullptr;
        CheckCom(moniker->GetDisplayName(nullptr, nullptr, &display), "Read camera identifier");
        std::string name;
        try { name = Utf8(display); } catch (...) { CoTaskMemFree(display); throw; }
        CoTaskMemFree(display);
        std::replace(name.begin(), name.end(), ':', '_');
        std::string description = name;
        ComPtr<IPropertyBag> properties;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties)))) {
            VARIANT value; VariantInit(&value);
            if (SUCCEEDED(properties->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR && value.bstrVal) {
                try { description = Utf8(value.bstrVal); } catch (...) { VariantClear(&value); throw; }
            }
            VariantClear(&value);
        }
        result.push_back({std::move(name), std::move(description)});
    }
    CheckCom(next, "Read camera enumeration");
    return result;
}
// 将FFmpeg唯一名与DirectShow moniker对应；按输出引脚读取每项真实格式和帧间隔。
std::vector<CameraMode> ListCameraModes(const std::string& deviceName) {
    Apartment apartment;
    ComPtr<ICreateDevEnum> devices;
    CheckCom(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devices)), "Create camera enumerator");
    ComPtr<IEnumMoniker> list;
    HRESULT status = devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &list, 0);
    CheckCom(status, "Enumerate camera monikers");
    if (status == S_FALSE || !list) throw std::runtime_error("No camera device / 未找到摄像头");
    ComPtr<IMoniker> moniker;
    while (list->Next(1, moniker.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        LPOLESTR display = nullptr;
        CheckCom(moniker->GetDisplayName(nullptr, nullptr, &display), "Read camera identifier");
        std::string unique;
        try { unique = Utf8(display); } catch (...) { CoTaskMemFree(display); throw; }
        CoTaskMemFree(display);
        std::replace(unique.begin(), unique.end(), ':', '_'); // FFmpeg dshow唯一名使用同样的转义。
        if (unique != deviceName) continue;
        ComPtr<IBaseFilter> filter;
        CheckCom(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&filter)), "Open camera capabilities (device busy or privacy permission)");
        ComPtr<IEnumPins> pins;
        CheckCom(filter->EnumPins(&pins), "Enumerate camera pins");
        std::vector<CameraMode> modes;
        ComPtr<IPin> pin;
        while (pins->Next(1, pin.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
            PIN_DIRECTION direction;
            CheckCom(pin->QueryDirection(&direction), "Read camera pin direction");
            if (direction != PINDIR_OUTPUT) continue;
            ComPtr<IAMStreamConfig> config;
            status = pin.As(&config);
            if (status == E_NOINTERFACE) continue;
            CheckCom(status, "Query camera stream capabilities");
            int count = 0, bytes = 0;
            CheckCom(config->GetNumberOfCapabilities(&count, &bytes), "Count camera modes");
            if (count < 0 || count > 4096 || bytes != sizeof(VIDEO_STREAM_CONFIG_CAPS))
                throw std::runtime_error("Unsupported camera capability structure");
            for (int i = 0; i < count; ++i) {
                VIDEO_STREAM_CONFIG_CAPS caps{};
                MediaType media;
                if (config->GetStreamCaps(i, &media.value, reinterpret_cast<BYTE*>(&caps)) != S_OK)
                    throw std::runtime_error("Read camera mode failed");
                const auto* type = media.value;
                if (!type || type->majortype != MEDIATYPE_Video || !type->pbFormat) continue;
                const BITMAPINFOHEADER* bitmap = nullptr;
                int64_t interval = 0;
                if (type->formattype == FORMAT_VideoInfo && type->cbFormat >= sizeof(VIDEOINFOHEADER)) {
                    const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(type->pbFormat);
                    bitmap = &info->bmiHeader; interval = info->AvgTimePerFrame;
                } else if (type->formattype == FORMAT_VideoInfo2 && type->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
                    const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(type->pbFormat);
                    bitmap = &info->bmiHeader; interval = info->AvgTimePerFrame;
                }
                if (!bitmap || bitmap->biWidth <= 0 || bitmap->biHeight == LONG_MIN || !bitmap->biHeight || interval <= 0) continue;
                CameraMode mode;
                mode.width = bitmap->biWidth; mode.height = std::abs(bitmap->biHeight); mode.frameInterval = interval;
                DescribeFormat(type->subtype, mode);
                std::ostringstream name; name.imbue(std::locale::classic());
                name << mode.width << 'x' << mode.height << " @ " << std::fixed << std::setprecision(3) << 10000000.0 / interval
                    << " fps (interval=" << interval << ") / " << (mode.supported ? (mode.pixelFormat.empty() ? mode.codec : mode.pixelFormat) : "unsupported subtype")
                    << " / range " << (caps.MaxFrameInterval > 0 ? 10000000.0 / caps.MaxFrameInterval : 0)
                    << ".." << (caps.MinFrameInterval > 0 ? 10000000.0 / caps.MinFrameInterval : 0);
                mode.description = name.str(); modes.push_back(std::move(mode));
            }
        }
        return modes;
    }
    throw std::runtime_error("Camera identifier no longer present; refresh devices");
}
}
