// 设备/文件输入 → 解码 → WavWriter；媒体资源由调用线程局部拥有。
#include "AudioRecorder.h"
#include "../../media/ffmpeg/FfmpegSupport.h"
#include "../../media/mux/WavWriter.h"
namespace miniclip {
namespace {
int Interrupt(void* opaque) {
    return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}
}
CaptureResult RunAudioCapture(const CaptureOptions& options,
    const std::atomic<bool>& cancel, std::atomic<int64_t>& progress) {
    CaptureResult result;
    try {
        // 启动前已取消就不碰设备；每次任务有独立标志，worker不会重置用户的停止请求。
        if (cancel.load()) { result.cancelled = true; result.message = "Cancelled before opening input"; return result; }
        avdevice_register_all();
        InputResources input;
        input.format = avformat_alloc_context();
        if (!input.format) throw std::bad_alloc();
        // 设置辅助中断回调，opaque借用cancel地址；const_cast仅满足C接口的void*形状。
        input.format->interrupt_callback = {Interrupt, const_cast<std::atomic<bool>*>(&cancel)};
        // 同一处理链路兼容设备与文件实验；live为true时使用DirectShow，文件则自动识别输入格式。
        bool live = options.inputFile.empty();
        const AVInputFormat* format = live ? av_find_input_format("dshow") : nullptr;
        if (live && !format) throw std::runtime_error("DirectShow input unavailable");
        // 非阻塞设备读取：暂时没有数据返回EAGAIN，允许循环检查取消，而不是无限等一包。
        if (live) input.format->flags |= AVFMT_FLAG_NONBLOCK;
        // 目标设备缓冲为50ms；实际参数与退出延迟仍须在真实驱动上测量。
        if (live) Check(av_dict_set_int(&input.options, "audio_buffer_size", 50, 0), "Set device buffer");
        std::string url = live ? "audio=" + options.device : options.inputFile;
        // &input.format可写回上下文，&input.options可写回未消费选项；失败按SDK约定清理上下文。
        Check(avformat_open_input(&input.format, url.c_str(), format, &input.options), "Open input");
        // 剩余选项表明输入没有接受它们，当前不悄悄忽略用户以为已生效的参数。
        if (av_dict_count(input.options)) throw std::runtime_error("Input did not accept device options");
        // 文件输入额外探测流信息；实时设备已有参数，避免在此新增阻塞探测。
        if (!live) Check(avformat_find_stream_info(input.format, nullptr), "Read stream information");
        // 设备打开可能耗时，返回后再次检查，处理“启动过程中点击停止”的情况。
        if (cancel.load()) { result.cancelled = true; result.message = "Cancelled while opening input"; return result; }
        // 寻找第一条音频流；-1表示尚未找到，避免默认把任意流都按音频解码。
        int streamIndex = -1;
        for (unsigned i = 0; i < input.format->nb_streams; ++i)
            if (input.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { streamIndex = static_cast<int>(i); break; }
        if (streamIndex < 0) throw std::runtime_error("No audio stream");
        const AVCodecParameters* parameters = input.format->streams[streamIndex]->codecpar;
        std::printf("Input codec: %s, declared sample rate: %d, channels: %d\n",
            avcodec_get_name(parameters->codec_id), parameters->sample_rate, parameters->ch_layout.nb_channels);
        // 以实际codec_id找解码器；PCM也统一经过对应解码器，得到后续可统一处理的AVFrame。
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) throw std::runtime_error("Audio decoder unavailable");
        input.decoder = avcodec_alloc_context3(codec);
        if (!input.decoder) throw std::bad_alloc();
        // 复制流参数到解码运行上下文，再open2初始化；参数描述不是解码器本身。
        Check(avcodec_parameters_to_context(input.decoder, parameters), "Set decoder parameters");
        Check(avcodec_open2(input.decoder, codec, nullptr), "Open decoder");
        // 输出对象是worker局部所有者，异常时析构；先Open，后循环Push，最后Finish。
        WavWriter writer(options.outputFile);
        writer.Open();
        Packet packet = NewPacket();
        Frame frame = NewFrame();
        // 局部lambda按引用捕获仍存活的上下文/帧/输出对象；在本函数内同步调用，不交到函数外。
        auto receive = [&]() {
            for (;;) {
                // 一个包可能产出多帧，也可能暂时不产帧，因此循环取到EAGAIN/EOF为止。
                int ret = avcodec_receive_frame(input.decoder, frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
                Check(ret, "Decode audio");
                writer.Push(frame.get());
                // 只通过atomic更新GUI可并发读取的进度；帧处理完成后解除本次数据引用。
                progress.store(writer.Samples());
                av_frame_unref(frame.get());
            }
        };
        // 发送包若遇EAGAIN，先推进接收端，再重试原包；成功后接收所有当前可用帧。
        auto send = [&](const AVPacket* p) {
            int ret = avcodec_send_packet(input.decoder, p);
            // 暂无包不是致命错误：短暂休眠避免空转，超过10秒无音频则报错。休眠不等于线程同步。
            if (ret == AVERROR(EAGAIN)) { receive(); ret = avcodec_send_packet(input.decoder, p); }
            Check(ret, "Send decoder packet");
            receive();
        };
        // steady_clock用于耗时/无数据超时，避免系统日历时间调整影响；音频时长另用样本数。
        auto started = std::chrono::steady_clock::now();
        auto lastData = started;
        // 每轮先检查协作式取消；终止循环不等于文件已完成，后面还要排空与收尾。
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
            // 普通文件EOF是正常结束；设备异常与未取消的其他错误仍需报告。
            if (ret == AVERROR_EOF && !live) break;
            if (ret < 0 && cancel.load()) break;
            Check(ret, "Read audio packet");
            // 只发送已选中的音频流；处理后unref包内容，保留包对象继续读。
            if (packet->stream_index == streamIndex) { send(packet.get()); lastData = std::chrono::steady_clock::now(); }
            av_packet_unref(packet.get());
        }
        // 停止输入后通知解码器结束并取完尾部帧，然后由writer继续排空转换和编码。
        send(nullptr);
        writer.Finish();
        result.samples = writer.Samples();
        progress.store(result.samples);
        // 仅Finish正常返回后记录成功；最终样本数可能比循环中进度多出排空的尾部样本。
        result.success = true;
        result.message = "WAV saved: " + Utf8(options.outputFile);
    // 无样本单独分类；若用户确实请求取消，则返回取消而不发布空WAV。
    } catch (const NoSamples& error) {
        result.cancelled = cancel.load();
        result.message = error.what();
    } catch (const std::exception& error) {
        // 即使已经按下停止，写入/收尾失败仍是失败，不能伪装成正常取消。
        result.message = error.what();
    }
    return result;
}
// 独立验证入口，不使用麦克风；生成已知float planar信号，通过真实WavWriter写出。
CaptureResult WriteSyntheticAudio(const std::wstring& outputFile, bool stereo) {
    CaptureResult result;
    try {
        WavWriter writer(outputFile);
        writer.Open();
        Frame frame = NewFrame();
        // 明确输入格式后才允许按float访问平面；441样本×100块/44100Hz=1秒。
        frame->format = AV_SAMPLE_FMT_FLTP;
        frame->sample_rate = 44100;
        frame->nb_samples = 441;
        av_channel_layout_default(&frame->ch_layout, stereo ? 2 : 1);
        Check(av_frame_get_buffer(frame.get(), 0), "Allocate test samples");
        for (int block = 0; block < 100; ++block) {
            // 写入前确保缓冲可写，防止共享引用条件下覆盖仍被其他对象使用的数据。
            Check(av_frame_make_writable(frame.get()), "Make test frame writable");
            for (int channel = 0; channel < frame->ch_layout.nb_channels; ++channel) {
                // 左右分别440/880Hz；测试可发现漏掉第二平面、复制或交换声道的问题。
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

}
