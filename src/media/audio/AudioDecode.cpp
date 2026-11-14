// 这里实现S04播放的声音准备过程：AudioPlayer::Produce调用DecodeAudioFile，libavformat从文件拆出音轨包，
// libavcodec把包解码成样本，libswresample再统一规格，最后分成PCM块交回播放队列。
// 输入、解码器、帧/包和swr都由这个生产线程拥有；交出去的块是独立值，不依赖这些FFmpeg资源继续存活。
#define NOMINMAX
#include "AudioDecode.h"
#include "../ffmpeg/FfmpegSupport.h"
#include <filesystem>
#include <vector>

namespace miniclip {
namespace {
// DecodeAudioFile把它登记为FFmpeg中断回调，读取AudioPlayer仍持有的停止标志。
// 用户停止时返回1，让支持中断的输入操作尽早返回；这里不释放正在使用的输入资源。
int InterruptPlayback(void* opaque) {
    return static_cast<const std::atomic<bool>*>(opaque)->load() ? 1 : 0;
}
// DecodeAudioFile及其处理循环在步骤之间调用它；发现停止就抛PlaybackCancelled，
// 由栈上对象先释放输入/转换资源，再让播放层按“已停止”处理，而不是继续产生样本。
void CheckStop(const std::atomic<bool>& stop) {
    if (stop.load()) throw PlaybackCancelled();
}
// 重采样器拥有FFmpeg缓冲；源格式描述也需要自己的引用副本。
struct AudioConversion {
    SwrContext* context = nullptr;
    AVChannelLayout layout{};
    int rate = 0;
    int format = AV_SAMPLE_FMT_NONE;
    // DecodeAudioFile结束或异常离开时，归还swr内部缓冲和本对象复制的源声道布局。
    ~AudioConversion() { swr_free(&context); av_channel_layout_uninit(&layout); }
    // 先保持空状态，收到首个真实音频帧后才能确定swr应使用的输入规格。
    AudioConversion() = default;
    // swr只归这个转换对象所有，不能复制后被两份对象重复释放。
    AudioConversion(const AudioConversion&) = delete;
    // 赋值同样不能复制转换器和声道布局的所有权。
    AudioConversion& operator=(const AudioConversion&) = delete;
};
}

// AudioPlayer::Produce在生产线程调用这个入口。先open_input/find_stream_info找首音轨并打开对应解码器，
// 然后read_frame取包、send/receive还原样本，swr_convert统一成48k/float/stereo，再由consume接收PCM块。
// 读到EOF还要排空解码器和swr；明确停止或其他失败会抛给调用方，局部资源随作用域自动归还。
AudioDecodeResult DecodeAudioFile(const std::wstring& file, const std::atomic<bool>& stop,
    const std::function<void(PcmBlock)>& consume) {
    CheckStop(stop);
    if (!consume) throw std::invalid_argument("Missing PCM consumer");
    if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("Playback requires an existing local file / 请选择本地文件");
    // 先设置中断回调和file协议白名单，再打开/探测输入，避免将文件路径当网络协议使用。
    InputResources input;
    input.format = avformat_alloc_context();
    if (!input.format) throw std::bad_alloc();
    input.format->interrupt_callback = {InterruptPlayback, const_cast<std::atomic<bool>*>(&stop)};
    Check(av_dict_set(&input.options, "protocol_whitelist", "file", 0), "Restrict playback to local files");
    int ret = avformat_open_input(&input.format, Utf8(file).c_str(), nullptr, &input.options);
    CheckStop(stop);
    Check(ret, "Open playback file");
    ret = avformat_find_stream_info(input.format, nullptr);
    CheckStop(stop);
    Check(ret, "Read playback stream information");
    // 找到第一条音轨，再按其codec_id打开解码器；后面的读包循环只向它发送这条轨道的包。
    AudioDecodeResult result;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        if (input.format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            result.streamIndex = static_cast<int>(i); break;
        }
    }
    if (result.streamIndex < 0) throw std::runtime_error("No audio stream / 文件中没有音轨");
    const AVStream* stream = input.format->streams[result.streamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) throw std::runtime_error("Playback audio decoder unavailable");
    input.decoder = avcodec_alloc_context3(codec);
    if (!input.decoder) throw std::bad_alloc();
    Check(avcodec_parameters_to_context(input.decoder, stream->codecpar), "Configure playback decoder");
    input.decoder->pkt_timebase = stream->time_base;
    input.decoder->thread_count = 1;
    input.decoder->err_recognition = AV_EF_EXPLODE;
    Check(avcodec_open2(input.decoder, codec, nullptr), "Open playback decoder");
    AudioConversion conversion;
    Packet packet = NewPacket();
    Frame frame = NewFrame();
    std::vector<float> converted;
    PcmBlock block;
    // receive和尾部排空用deliver整理转换结果：先补满当前1024帧块，再调用consume交出去。
    // 不足一块的部分保留到下一批或EOF，累计长度只算真实样本，不为填满数组补入假时长。
    auto deliver = [&](int frames) {
        int offset = 0;
        while (offset < frames) {
            CheckStop(stop);
            const int count = std::min(PcmBlockFrames - block.frames, frames - offset);
            std::copy_n(converted.data() + offset * PlaybackChannels, count * PlaybackChannels,
                block.samples.data() + block.frames * PlaybackChannels);
            offset += count;
            block.frames += count;
            result.frames += count;
            if (block.frames == PcmBlockFrames) { consume(std::move(block)); block = PcmBlock{}; }
        }
    };
    // send每次交包后调用receive，循环取完当前可用解码帧，按真实源格式建立/检查swr，再转换并交给deliver。
    // 一包可能产出多帧或暂时不产帧，所以必须持续接收直到EAGAIN或EOF，而不是只收一次。
    auto receive = [&]() {
        for (;;) {
            CheckStop(stop);
            const int code = avcodec_receive_frame(input.decoder, frame.get());
            if (code == AVERROR(EAGAIN) || code == AVERROR_EOF) return;
            Check(code, "Decode playback audio");
            // 明确首版范围/分配上限。过大或中途变化的格式报错，不悄悄误读。
            if (frame->sample_rate < 8000 || frame->sample_rate > 192000 ||
                frame->ch_layout.nb_channels < 1 || frame->ch_layout.nb_channels > 2 ||
                !av_channel_layout_check(&frame->ch_layout) || frame->nb_samples <= 0 || frame->nb_samples > 65536)
                throw std::runtime_error("Playback supports fixed mono/stereo, 8–192kHz and bounded audio frames");
            if (!conversion.context) {
                conversion.rate = frame->sample_rate;
                conversion.format = frame->format;
                Check(av_channel_layout_copy(&conversion.layout, &frame->ch_layout), "Copy playback layout");
                AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                Check(swr_alloc_set_opts2(&conversion.context, &stereo, AV_SAMPLE_FMT_FLT, PlaybackRate,
                    &conversion.layout, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr), "Create playback resampler");
                Check(swr_init(conversion.context), "Initialize playback resampler");
            } else if (conversion.rate != frame->sample_rate || conversion.format != frame->format ||
                av_channel_layout_compare(&conversion.layout, &frame->ch_layout) != 0)
                throw std::runtime_error("Audio format changed during playback / 暂不支持中途变更格式");
            const int64_t capacity = av_rescale_rnd(swr_get_delay(conversion.context, conversion.rate) + frame->nb_samples,
                PlaybackRate, conversion.rate, AV_ROUND_UP);
            if (capacity <= 0 || capacity > 500000) throw std::runtime_error("Playback conversion buffer limit exceeded");
            converted.resize(static_cast<size_t>(capacity) * PlaybackChannels);
            uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
            // extended_data兼容planar多平面；FLT目标只有一个交错平面。
            const int produced = swr_convert(conversion.context, output, static_cast<int>(capacity),
                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
            Check(produced, "Convert playback samples");
            deliver(produced);
            av_frame_unref(frame.get());
        }
    };
    // 主读包循环用send推进解码：若send_packet返回EAGAIN，先receive取结果，再重送同一个包。
    // 发送成功后继续接收可用帧，不能把“暂时塞不进去”误当成包已被接收并直接丢掉。
    auto send = [&](const AVPacket* value) {
        int code = avcodec_send_packet(input.decoder, value);
        if (code == AVERROR(EAGAIN)) { receive(); code = avcodec_send_packet(input.decoder, value); }
        Check(code, "Send playback packet");
        receive();
    };
    // 顺序读包，只解码选中的音轨；每轮unref包内容后复用对象，文件里的视频包在这里跳过。
    for (;;) {
        CheckStop(stop);
        ret = av_read_frame(input.format, packet.get());
        CheckStop(stop);
        if (ret == AVERROR_EOF) break;
        Check(ret, "Read playback packet");
        if (packet->stream_index == result.streamIndex) send(packet.get());
        av_packet_unref(packet.get());
    }
    // 读到EOF先发空包取出解码器尾部，再以空输入排出swr延迟样本，最后交当前短块，防止声音结尾被截掉。
    send(nullptr);
    if (!conversion.context) throw std::runtime_error("No decoded audio samples / 没有可播放样本");
    converted.resize(PcmBlockFrames * PlaybackChannels);
    for (;;) {
        CheckStop(stop);
        uint8_t* output[] = {reinterpret_cast<uint8_t*>(converted.data())};
        const int produced = swr_convert(conversion.context, output, PcmBlockFrames, nullptr, 0);
        Check(produced, "Drain playback resampler");
        if (produced == 0) break;
        deliver(produced);
    }
    if (block.frames) consume(std::move(block));
    return result;
}
}
