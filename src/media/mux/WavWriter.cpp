// WAV转换/编码/封装实现；保持原正常数据流，补充调用顺序检查。
#include "WavWriter.h"
namespace miniclip {
WavWriter::WavWriter(const std::wstring& destination) : destination_(destination) {
        if (destination.empty()) throw std::runtime_error("Empty output path");
        // 先写同目录临时文件；仅当收尾成功才改为用户目标名，避免半成品显示为成功文件。
		temporary_ = destination + L".part-" + std::to_wstring(GetCurrentProcessId())  //临时文件名包含进程ID，避免多个实例冲突
			+ L"-" + std::to_wstring(GetTickCount64());    //加启动时间戳，避免同一进程多次录音冲突
    }

WavWriter::~WavWriter() {
		swr_free(&resampler_);  //释放重采样器，内部可能有缓冲区和滤波器状态。
		av_channel_layout_uninit(&inputLayout_);  //释放输入声道布局，避免内存泄漏。
		avcodec_free_context(&encoder_);   //释放编码器上下文，避免内存泄漏。
        if (output_) {
            if (output_->pb) {
                int ret = avio_closep(&output_->pb);
                if (ret < 0) std::fprintf(stderr, "Output cleanup close error: %d\n", ret);
            }
            avformat_free_context(output_);
        }
        // 尚未发布则清理临时文件；清理失败记录诊断。强制结束/断电不保证执行析构。
        if (!published_ && !DeleteFileW(temporary_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            std::fprintf(stderr, "Partial file cleanup error: %lu\n", GetLastError());
    }

void WavWriter::Open() {
    if (openAttempted_) throw std::logic_error("WavWriter::Open called twice");
    openAttempted_ = true;
        std::string path = Utf8(temporary_);
        // 明确指定wav格式，因为临时文件扩展名不是.wav；&output_允许函数写回新指针。
        Check(avformat_alloc_output_context2(&output_, nullptr, "wav", path.c_str()), "Create WAV");
        if (!output_) throw std::bad_alloc();
        // 查找实现描述是借用，不自行释放；真正有运行状态的是随后分配的encoder_。
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        if (!codec) throw std::runtime_error("PCM S16LE encoder unavailable");
        encoder_ = avcodec_alloc_context3(codec);
        if (!encoder_) throw std::bad_alloc();
        // 输出目标：每声道每秒48000样本、16位有符号整数、左右双声道。输入仍按真实参数读取。
        encoder_->sample_rate = 48000;
        encoder_->sample_fmt = AV_SAMPLE_FMT_S16;
        av_channel_layout_default(&encoder_->ch_layout, 2);
        // 时间单位为1/48000秒；例如PTS=480代表0.01秒。这里不是字节计数。
        encoder_->time_base = AVRational{1, 48000};
        // 按位检查容器是否要求独立的编码头信息；|=保留其他标志并设置对应位。
        if (output_->oformat->flags & AVFMT_GLOBALHEADER) encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        Check(avcodec_open2(encoder_, codec, nullptr), "Open PCM encoder");
        // 输出流随output_管理，stream_只是借用。复制编码参数使容器知道如何描述输出数据。
        stream_ = avformat_new_stream(output_, nullptr);
        if (!stream_) throw std::bad_alloc();
        stream_->time_base = encoder_->time_base;
        Check(avcodec_parameters_from_context(stream_->codecpar, encoder_), "Set output parameters");
        // 先打开临时文件，再写header；参数设定成功不代表磁盘写入一定成功。
        Check(avio_open(&output_->pb, path.c_str(), AVIO_FLAG_WRITE), "Open output file");
        Check(avformat_write_header(output_, nullptr), "Write WAV header");
        opened_ = true;
    }

void WavWriter::Push(const AVFrame* input) {
    if (!opened_ || finishing_) throw std::logic_error("WavWriter::Push outside recording");
    if (!input) throw std::invalid_argument("WavWriter::Push requires an audio frame");
        if (input->sample_rate <= 0 || input->nb_samples <= 0 || !av_channel_layout_check(&input->ch_layout))
            throw std::runtime_error("Invalid decoded audio parameters");
        // 第一帧才知道真实样本格式/布局，用它建立持续使用的重采样器，不能每帧丢掉转换状态。
        if (!resampler_) {
            inputRate_ = input->sample_rate;
            inputFormat_ = input->format;
            Check(av_channel_layout_copy(&inputLayout_, &input->ch_layout), "Copy input layout");
            // 参数依次为：转换器地址、输出布局/格式/采样率、输入布局/格式/采样率、日志设置。
            Check(swr_alloc_set_opts2(&resampler_, &encoder_->ch_layout, encoder_->sample_fmt, 48000,
                &inputLayout_, static_cast<AVSampleFormat>(inputFormat_), inputRate_, 0, nullptr), "Allocate resampler");
            Check(swr_init(resampler_), "Initialize resampler");
            char layout[128] = {};
            Check(av_channel_layout_describe(&inputLayout_, layout, sizeof(layout)), "Describe layout");
            const char* format = av_get_sample_fmt_name(static_cast<AVSampleFormat>(inputFormat_));
            std::printf("Decoded input: %d Hz, %s, %s -> 48000 Hz, s16, stereo\n",
                inputRate_, format ? format : "unknown", layout);
        // 后续参数变化则明确失败；动态重配置还没有实现，不能继续用旧格式解释新数据。
        } else if (inputRate_ != input->sample_rate || inputFormat_ != input->format ||
            av_channel_layout_compare(&inputLayout_, &input->ch_layout) != 0) {
            throw std::runtime_error("Input format changed during recording; restart recording");
        }
        Convert(input);
    }

void WavWriter::Finish() {
    if (!opened_ || finishing_) throw std::logic_error("WavWriter::Finish outside recording");
    finishing_ = true;
        // 空输入让转换器把滤波器中延迟的样本输出完；否则可能截掉结尾音频。（排空转换器即重采样器）
        if (resampler_) while (Convert(nullptr) > 0) {}
        if (samples_ == 0) throw NoSamples();
        // 空帧向编码器表示输入结束，取出内部剩余编码包。
        Send(nullptr);
        Check(av_write_trailer(output_), "Write WAV trailer");
        Check(avio_closep(&output_->pb), "Close output file");
        // 不覆盖已有文件，也不覆盖选择路径后被其他程序创建的文件。
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Publish WAV failed (destination may exist), Windows error " + std::to_string(GetLastError()));
        // 只有最终发布成功才设置标志；若任何一步抛异常，析构仍按失败路径处理临时文件。
        published_ = true;
    }

int64_t WavWriter::Samples() const { return samples_; }

int WavWriter::Convert(const AVFrame* input) {
        // 容量向上舍入：(内部延迟+输入样本)×目标采样率/输入采样率。容量不是实际有效数据量。
        int64_t capacity = av_rescale_rnd(swr_get_delay(resampler_, inputRate_)
            + (input ? input->nb_samples : 0), 48000, inputRate_, AV_ROUND_UP);
        if (capacity > INT_MAX) throw std::runtime_error("Audio buffer too large");
        // 智能指针拥有本次输出帧；函数结束或中途异常都会释放。此实现仍有重复分配，尚待测量优化。
        Frame frame = NewFrame();
        frame->format = AV_SAMPLE_FMT_S16;
        frame->sample_rate = 48000;
        // 至少准备256样本空间便于排空；这不表示每次一定输出256。括号写法避免Windows max宏干扰。
        frame->nb_samples = static_cast<int>((std::max)(capacity, int64_t{256}));
        Check(av_channel_layout_copy(&frame->ch_layout, &encoder_->ch_layout), "Copy output layout");
        Check(av_frame_get_buffer(frame.get(), 0), "Allocate output samples");
        // extended_data是一组声道/平面地址，支持planar；不是只拷贝data[0]。
        // 这里的const_cast匹配C接口，不会把数据内容转换成另一种音频格式；转换由swr完成。
        int produced = swr_convert(resampler_, frame->extended_data, frame->nb_samples,
			input ? const_cast<const uint8_t**>(input->extended_data) : nullptr, input ? input->nb_samples : 0);   //执行重采样转换，返回实际产生的每声道样本数。输入为空表示排空。
        Check(produced, "Resample audio");
        if (produced) {
            // 只编码实际有效样本；若把容量当长度，会将无效缓冲尾部也作为音频。
            frame->nb_samples = produced;
            // 新帧从此前累计样本位置开始；送入编码器成功后再累加本帧样本数。
            frame->pts = samples_;
            Send(frame.get());
            samples_ += produced;
        }
		return produced; //返回实际产生的每声道样本数。输入为空表示排空。
    }

void WavWriter::Receive() {
        Packet packet = NewPacket();
        for (;;) {
            int ret = avcodec_receive_packet(encoder_, packet.get()); //从编码器中取一个编码包
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            Check(ret, "Receive PCM packet");
            packet->stream_index = stream_->index;
            // 把编码器时间单位换算为输出流时间单位，不能直接假定所有环节单位相同。
            av_packet_rescale_ts(packet.get(), encoder_->time_base, stream_->time_base);
            // 该写包接口会消耗包引用；包对象继续由unique_ptr管理，随后unref空引用安全。
            Check(av_interleaved_write_frame(output_, packet.get()), "Write WAV packet");
            av_packet_unref(packet.get());
        }
    }

void WavWriter::Send(const AVFrame* frame) {
        int ret = avcodec_send_frame(encoder_, frame);
        if (ret == AVERROR(EAGAIN)) { Receive(); ret = avcodec_send_frame(encoder_, frame); }
        Check(ret, "Send PCM frame");
        Receive();
    }
}
