// 这个文件实现S00录音及音频转WAV实验。RecordingController在后台调用它服务窗口，
// --record/--convert等CLI也复用这里：先打开输入、解码声音，再交WavWriter转换并保存。
// 每次调用独占自己的媒体资源，与S03视频转码、S04向扬声器播放是不同的处理出口。
#include "AudioRecorder.h"
#include "../../media/ffmpeg/FfmpegSupport.h"
#include "../../media/mux/WavWriter.h"
namespace miniclip {
namespace {
// RunAudioCapture把它登记为输入中断回调，FFmpeg读取时可通过它查询cancel。
// 它只读取借用的原子标志、告诉FFmpeg是否请求停止，不能强制打断任意驱动操作。
int Interrupt(void* opaque) {
    return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}
}
// RecordingController或CLI调用这里完成一次S00录音/转WAV。先打开输入、找到音轨并初始化解码器，
// 再反复读取包、送解码器、取声音帧，交给WavWriter::Push；结束时排空尾部并Finish保存。
// 停止请求通常是结束读取并保存已有内容，真正写完才报告成功；异常转成CaptureResult交上层显示。
// 这些工作在调用线程内进行，输入/解码器/帧包/writer也归它独占；GUI只能并发改cancel、读progress。
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
        // opaque借用cancel到函数返回；const_cast仅适配C回调形状，不改变只读约定。
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
        // 选好设备或文件路径后调用avformat_open_input，它会写回输入上下文及未消费的选项。
        Check(avformat_open_input(&input.format, url.c_str(), format, &input.options), "Open input");
        // 剩余选项表明输入没有接受它们，当前不悄悄忽略用户以为已生效的参数。
        if (av_dict_count(input.options)) throw std::runtime_error("Input did not accept device options");
        // 文件输入额外探测流信息；实时设备已有参数，避免在此新增阻塞探测。
        if (!live) Check(avformat_find_stream_info(input.format, nullptr), "Read stream information");
        // 设备打开可能耗时，返回后再次检查，处理“启动过程中点击停止”的情况。
        if (cancel.load()) { result.cancelled = true; result.message = "Cancelled while opening input"; return result; }
        // 输入打开后，先挑出第一条音轨，再按它的真实codec_id建立解码器，不能把任意包都视为PCM。
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
        // avcodec_parameters_to_context复制描述，avcodec_open2才初始化可工作的解码器。
        Check(avcodec_parameters_to_context(input.decoder, parameters), "Set decoder parameters");
        Check(avcodec_open2(input.decoder, codec, nullptr), "Open decoder");
        // 解码器准备好后再打开WavWriter；它拥有转换/编码/封装资源，后续先Push声音，最后Finish发布。
        WavWriter writer(options.outputFile);
        writer.Open();
        Packet packet = NewPacket();
        Frame frame = NewFrame();
        // send每送完一个包会调用这个lambda，把解码器当前可给的帧全部取出，交writer.Push后更新进度。
        // 一包不一定对应一帧，必须取到EAGAIN/EOF才停；帧处理完就unref，引用不会逃出本次调用。
        auto receive = [&]() {
            for (;;) {
                int ret = avcodec_receive_frame(input.decoder, frame.get());
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
                Check(ret, "Decode audio");
                writer.Push(frame.get());
                // 只通过atomic更新GUI可并发读取的进度；帧处理完成后解除本次数据引用。
                progress.store(writer.Samples());
                av_frame_unref(frame.get());
            }
        };
        // 主循环和结尾排空都会通过这个lambda送包；解码器返回EAGAIN时，先receive取走结果再重送原包。
        // 送入成功后继续receive，把本轮声音交给writer；不能直接跳过原包，否则会丢失内容。
        auto send = [&](const AVPacket* p) {
            int ret = avcodec_send_packet(input.decoder, p);
            if (ret == AVERROR(EAGAIN)) { receive(); ret = avcodec_send_packet(input.decoder, p); }
            Check(ret, "Send decoder packet");
            receive();
        };
        // steady_clock用于耗时/无数据超时，避免系统日历时间调整影响；音频时长另用样本数。
        auto started = std::chrono::steady_clock::now();
        auto lastData = started;
        // 每轮先检查停止和录音时限，再用av_read_frame取包，只将所选音轨交给send，处理后解除包引用。
        // 设备暂时无数据就短等并检查超时；跳出读取循环还不等于WAV已经保存，后面仍须排空收尾。
        while (!cancel.load()) {
            if (live && options.durationSeconds > 0 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= options.durationSeconds) break;
            int ret = av_read_frame(input.format, packet.get());
            // 设备暂时无包时短等；这与上面解码器要求推进接收端的EAGAIN不同。
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
        // 不再读取输入后，send(nullptr)取完解码尾部，writer.Finish再排转换/编码、补文件信息并发布。
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
// --synthetic命令和自动验证调用这里，不接麦克风也能检查S00的声音保存链路。
// 它先Open写入器，分配44.1kHz的FLTP帧，填充100块、每块441样本后Push，最后Finish得到1秒WAV。
// FLTP各声道分开存；双声道时左右分别为440/880Hz，用已知频率发现漏读平面、复制或交换声道。
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
