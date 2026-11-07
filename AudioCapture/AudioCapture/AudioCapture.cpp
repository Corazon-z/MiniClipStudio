#include "AudioCapture.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <chrono>
// C 链接约定，用于链接 FFmpeg C API，不是支持函数重载。
extern "C" {
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
}
namespace {
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode path");
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr))
        throw std::runtime_error("Path conversion failed");
    return result;
}
void Check(int code, const char* stage) {
    if (code >= 0) return;
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, text, sizeof(text)) < 0) strcpy_s(text, "Unknown FFmpeg error");
    throw std::runtime_error(std::string(stage) + ": " + text);
}
struct FrameDelete { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDelete { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct NoSamples : std::runtime_error {
    NoSamples() : std::runtime_error("No audio samples recorded; no WAV file was saved") {}
};
using Frame = std::unique_ptr<AVFrame, FrameDelete>;
using Packet = std::unique_ptr<AVPacket, PacketDelete>;
Frame NewFrame() {
    Frame p(av_frame_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}
Packet NewPacket() {
    Packet p(av_packet_alloc());
    if (!p) throw std::bad_alloc();
    return p;
}
struct InputResources {
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVDictionary* options = nullptr;
    ~InputResources() {
        av_dict_free(&options);
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
    }
};
// 输出资源的唯一所有者；异常时清理临时文件，成功收尾后才发布最终文件。
class WavWriter {
public:
    explicit WavWriter(const std::wstring& destination) : destination_(destination) {
        if (destination.empty()) throw std::runtime_error("Empty output path");
        temporary_ = destination + L".part-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64());
    }
    ~WavWriter() {
        swr_free(&resampler_);
        av_channel_layout_uninit(&inputLayout_);
        avcodec_free_context(&encoder_);
        if (output_) {
            if (output_->pb) {
                int ret = avio_closep(&output_->pb);
                if (ret < 0) std::fprintf(stderr, "Output cleanup close error: %d\n", ret);
            }
            avformat_free_context(output_);
        }
        if (!published_ && !DeleteFileW(temporary_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            std::fprintf(stderr, "Partial file cleanup error: %lu\n", GetLastError());
    }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    void Open() {
        std::string path = Utf8(temporary_);
        Check(avformat_alloc_output_context2(&output_, nullptr, "wav", path.c_str()), "Create WAV");
        if (!output_) throw std::bad_alloc();
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (!codec) throw std::runtime_error("PCM S16LE encoder unavailable");
        encoder_ = avcodec_alloc_context3(codec);
        if (!encoder_) throw std::bad_alloc();
        encoder_->sample_rate = 48000;
        encoder_->sample_fmt = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&encoder_->ch_layout, 2);
        encoder_->time_base = AVRational{1, 48000};
        if (output_->oformat->flags & AVFMT_GLOBALHEADER) encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        Check(avcodec_open2(encoder_, codec, nullptr), "Open PCM encoder");
        stream_ = avformat_new_stream(output_, nullptr);
        if (!stream_) throw std::bad_alloc();
        stream_->time_base = encoder_->time_base;
        Check(avcodec_parameters_from_context(stream_->codecpar, encoder_), "Set output parameters");
        Check(avio_open(&output_->pb, path.c_str(), AVIO_FLAG_WRITE), "Open output file");
        Check(avformat_write_header(output_, nullptr), "Write WAV header");
    }
    void Push(const AVFrame* input) {
        if (input->sample_rate <= 0 || input->nb_samples <= 0 || !av_channel_layout_check(&input->ch_layout))
            throw std::runtime_error("Invalid decoded audio parameters");
        if (!resampler_) {
            inputRate_ = input->sample_rate;
            inputFormat_ = input->format;
            Check(av_channel_layout_copy(&inputLayout_, &input->ch_layout), "Copy input layout");
            Check(swr_alloc_set_opts2(&resampler_, &encoder_->ch_layout, encoder_->sample_fmt, 48000,
                &inputLayout_, static_cast<AVSampleFormat>(inputFormat_), inputRate_, 0, nullptr), "Allocate resampler");
            Check(swr_init(resampler_), "Initialize resampler");
            char layout[128] = {};
            Check(av_channel_layout_describe(&inputLayout_, layout, sizeof(layout)), "Describe layout");
            const char* format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(inputFormat_));
            std::printf("Decoded input: %d Hz, %s, %s -> 48000 Hz, s16, stereo\n",
                inputRate_, format ? format : "unknown", layout);
        } else if (inputRate_ != input->sample_rate || inputFormat_ != input->format ||
            av_channel_layout_compare(&inputLayout_, &input->ch_layout) != 0) {
            throw std::runtime_error("Input format changed during recording; restart recording");
        }
        Convert(input);
    }
    void Finish() {
        if (resampler_) while (Convert(nullptr) > 0) {}
        if (samples_ == 0) throw NoSamples();
        Send(nullptr);
        Check(av_write_trailer(output_), "Write WAV trailer");
        Check(avio_closep(&output_->pb), "Close output file");
        // 不覆盖已有文件，也不覆盖选择路径后被其他程序创建的文件。
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Publish WAV failed (destination may exist), Windows error " + std::to_string(GetLastError()));
        published_ = true;
    }
    int64_t Samples() const { return samples_; }
private:
    int Convert(const AVFrame* input) {
        int64_t capacity = av_rescale_rnd(swr_get_delay(resampler_, inputRate_)
            + (input ? input->nb_samples : 0), 48000, inputRate_, AV_ROUND_UP);
        if (capacity > INT_MAX) throw std::runtime_error("Audio buffer too large");
        Frame frame = NewFrame();
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = 48000;
        frame->nb_samples = static_cast<int>((std::max)(capacity, int64_t{256}));
        Check(av_channel_layout_copy(&frame->ch_layout, &encoder_->ch_layout), "Copy output layout");
        Check(av_frame_get_buffer(frame.get(), 0), "Allocate output samples");
        int produced = swr_convert(resampler_, frame->extended_data, frame->nb_samples,
            input ? const_cast<const uint8_t**>(input->extended_data) : nullptr, input ? input->nb_samples : 0);
        Check(produced, "Resample audio");
        if (produced) {
            frame->nb_samples = produced;
            frame->pts = samples_;
            Send(frame.get());
            samples_ += produced;
        }
        return produced;
    }
    void Receive() {
        Packet packet = NewPacket();
        for (;;) {
            int ret = avcodec_receive_packet(encoder_, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            Check(ret, "Receive PCM packet");
            packet->stream_index = stream_->index;
            av_packet_rescale_ts(packet.get(), encoder_->time_base, stream_->time_base);
            Check(av_interleaved_write_frame(output_, packet.get()), "Write WAV packet");
            av_packet_unref(packet.get());
        }
    }
    void Send(const AVFrame* frame) {
        int ret = avcodec_send_frame(encoder_, frame);
        if (ret == AVERROR(EAGAIN)) { Receive(); ret = avcodec_send_frame(encoder_, frame); }
        Check(ret, "Send PCM frame");
        Receive();
    }
    std::wstring destination_, temporary_;
    AVFormatContext* output_ = nullptr;
    AVCodecContext* encoder_ = nullptr;
    AVStream* stream_ = nullptr; // 借用，随 output_ 释放
    SwrContext* resampler_ = nullptr;
    AVChannelLayout inputLayout_{};
    int inputRate_ = 0, inputFormat_ = -1;
    int64_t samples_ = 0;
    bool published_ = false;
};
int Interrupt(void* opaque) {
    return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}
} // namespace
std::vector<AudioDevice> ListAudioDevices() {
    avdevice_register_all();
    const AVInputFormat* format = av_find_input_format("dshow");
    if (!format) throw std::runtime_error("DirectShow input unavailable");
    AVDeviceInfoList* list = nullptr;
    int ret = avdevice_list_input_sources(format, nullptr, nullptr, &list);
    std::vector<AudioDevice> devices;
    try {
        Check(ret, "Enumerate DirectShow devices");
        for (int i = 0; list && i < list->nb_devices; ++i) {
            const AVDeviceInfo* info = list->devices[i];
            bool audio = false;
            for (int j = 0; j < info->nb_media_types; ++j)
                if (info->media_types[j] == AVMEDIA_TYPE_AUDIO) audio = true;
            if (audio && info->device_name) devices.push_back({info->device_name,
                info->device_description ? info->device_description : info->device_name});
        }
    } catch (...) { avdevice_free_list_devices(&list); throw; }
    avdevice_free_list_devices(&list);
    return devices;
}
CaptureResult RunAudioCapture(const CaptureOptions& options,
    const std::atomic<bool>& cancel, std::atomic<int64_t>& progress) {
    CaptureResult result;
    try {
        if (cancel.load()) { result.cancelled = true; result.message = "Cancelled before opening input"; return result; }
        avdevice_register_all();
        InputResources input;
        input.format = avformat_alloc_context();
        if (!input.format) throw std::bad_alloc();
        input.format->interrupt_callback = {Interrupt, const_cast<std::atomic<bool>*>(&cancel)};
        bool live = options.inputFile.empty();
        const AVInputFormat* format = live ? av_find_input_format("dshow") : nullptr;
        if (live && !format) throw std::runtime_error("DirectShow input unavailable");
        if (live) input.format->flags |= AVFMT_FLAG_NONBLOCK;
        // 目标设备缓冲为50ms；实际参数与退出延迟仍须在真实驱动上测量。
        if (live) Check(av_dict_set_int(&input.options, "audio_buffer_size", 50, 0), "Set device buffer");
        std::string url = live ? "audio=" + options.device : options.inputFile;
        Check(avformat_open_input(&input.format, url.c_str(), format, &input.options), "Open input");
        if (av_dict_count(input.options)) throw std::runtime_error("Input did not accept device options");
        if (!live) Check(avformat_find_stream_info(input.format, nullptr), "Read stream information");
        if (cancel.load()) { result.cancelled = true; result.message = "Cancelled while opening input"; return result; }
        int streamIndex = -1;
        for (unsigned i = 0; i < input.format->nb_streams; ++i)
            if (input.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { streamIndex = static_cast<int>(i); break; }
        if (streamIndex < 0) throw std::runtime_error("No audio stream");
        const AVCodecParameters* parameters = input.format->streams[streamIndex]->codecpar;
        std::printf("Input codec: %s, declared sample rate: %d, channels: %d\n",
            avcodec_get_name(parameters->codec_id), parameters->sample_rate, parameters->ch_layout.nb_channels);
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) throw std::runtime_error("Audio decoder unavailable");
        input.decoder = avcodec_alloc_context3(codec);
        if (!input.decoder) throw std::bad_alloc();
        Check(avcodec_parameters_to_context(input.decoder, parameters), "Set decoder parameters");
        Check(avcodec_open2(input.decoder, codec, nullptr), "Open decoder");
        WavWriter writer(options.outputFile);
        writer.Open();
        Packet packet = NewPacket();
        Frame frame = NewFrame();
        auto receive = [&]() {
            for (;;) {
                int ret = avcodec_receive_frame(input.decoder, frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
                Check(ret, "Decode audio");
                writer.Push(frame.get());
                progress.store(writer.Samples());
                av_frame_unref(frame.get());
            }
        };
        auto send = [&](const AVPacket* p) {
            int ret = avcodec_send_packet(input.decoder, p);
            if (ret == AVERROR(EAGAIN)) { receive(); ret = avcodec_send_packet(input.decoder, p); }
            Check(ret, "Send decoder packet");
            receive();
        };
        auto started = std::chrono::steady_clock::now();
        auto lastData = started;
        while (!cancel.load()) {
            if (live && options.durationSeconds > 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= options.durationSeconds) break;
            int ret = av_read_frame(input.format, packet.get());
            if (ret == AVERROR(EAGAIN)) {
                if (live && std::chrono::steady_clock::now() - lastData > std::chrono::seconds(10))
                    throw std::runtime_error("No audio data for 10 seconds; check device");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (ret == AVERROR_EOF && !live) break;
            if (ret < 0 && cancel.load()) break;
            Check(ret, "Read audio packet");
            if (packet->stream_index == streamIndex) { send(packet.get()); lastData = std::chrono::steady_clock::now(); }
            av_packet_unref(packet.get());
        }
        send(nullptr);
        writer.Finish();
        result.samples = writer.Samples();
        progress.store(result.samples);
        result.success = true;
        result.message = "WAV saved: " + Utf8(options.outputFile);
    } catch (const NoSamples& error) {
        result.cancelled = cancel.load();
        result.message = error.what();
    } catch (const std::exception& error) {
        // 即使已经按下停止，写入/收尾失败仍是失败，不能伪装成正常取消。
        result.message = error.what();
    }
    return result;
}
CaptureResult WriteSyntheticAudio(const std::wstring& outputFile, bool stereo) {
    CaptureResult result;
    try {
        WavWriter writer(outputFile);
        writer.Open();
        Frame frame = NewFrame();
        frame->format = AV_SAMPLE_FMT_FLTP;
        frame->sample_rate = 44100;
        frame->nb_samples = 441;
        av_channel_layout_default(&frame->ch_layout, stereo ? 2 : 1);
        Check(av_frame_get_buffer(frame.get(), 0), "Allocate test samples");
        for (int block = 0; block < 100; ++block) {
            Check(av_frame_make_writable(frame.get()), "Make test frame writable");
            for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
                float* samples = reinterpret_cast<float*>(frame->extended_data[channel]);
                for (int i = 0; i < frame->nb_samples; ++i)
                    samples[i] = 0.2f * static_cast<float>(std::sin(2 * 3.141592653589793 *
                        (channel == 0 ? 440 : 880) * (block * 441 + i) / 44100));
            }
            writer.Push(frame.get());
        }
        writer.Finish();
        result.success = true;
        result.samples = writer.Samples();
        result.message = "Synthetic planar conversion complete";
    } catch (const std::exception& error) { result.message = error.what(); }
    return result;
}
