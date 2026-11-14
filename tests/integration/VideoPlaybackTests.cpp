// 这里检查S05实际交付的像素/样本与播放状态。普通数值检查不用设备，播放检查使用真实WASAPI但静音。
// headless测试只观测调度邮箱，不冒充WM_PAINT、物理屏幕呈现或人工听感验收。
#define NOMINMAX
#include "VideoPlaybackTests.h"
#include "../../src/playback/VideoPlayer.h"
#include "../../src/media/ffmpeg/PreviewInput.h"
#include "../../src/media/video/VideoPreview.h"
#include "../../src/media/audio/AudioPreview.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <thread>
using namespace miniclip;
namespace {
// 测试条件不满足时立即向独立入口报告具体原因，不继续输出假成功。
void RequirePreview(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
// 截止时间只管测试等待；超时后对象仍必须停止并真实join，不能把超时当退出。
template<class Predicate> void AwaitPreview(Predicate done, const char* text) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(text);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
// 等待过程中先看失败，避免用后续“等画面超时”盖住真正的解码/设备错误。
void CheckVideoFailure(const VideoPlayer& player) {
    const auto status = player.Snapshot();
    if (status.state == PlaybackState::Failed) throw std::runtime_error(status.message);
}
// 已请求停止或自然结束后，轮询真正的线程回收；不用固定sleep猜测退出。
void JoinVideo(VideoPlayer& player) {
    AwaitPreview([&] { CheckVideoFailure(player); return player.TryJoin(); }, "Video thread did not exit");
}
}

// 打开输入取得共同原点，再分别调用两条同步解码链，保存它们真实交出的时间、像素和PCM。
void InspectPreviewDecoders(const std::wstring& file, double seconds, const std::wstring& prefix) {
    const std::filesystem::path jsonPath(prefix + L".json"), pcmPath(prefix + L".f32");
    RequirePreview(!std::filesystem::exists(jsonPath) && !std::filesystem::exists(pcmPath), "Preview inspection output already exists");
    std::atomic<bool> stop{false}; std::atomic<uint64_t> generation{7};
    PreviewReadControl control{stop, generation, 7};
    const PreviewSource source = ReadPreviewSource(file, control);
    const int64_t target = static_cast<int64_t>(std::llround(seconds * 1000000));
    std::ofstream json(jsonPath), pcm(pcmPath, std::ios::binary);
    RequirePreview(bool(json) && bool(pcm), "Cannot create preview inspection files");
    json.imbue(std::locale::classic());
    json << "{\"origin_us\":" << source.originUs << ",\"duration_us\":" << source.durationUs.value_or(-1)
        << ",\"video_stream\":" << source.videoStream << ",\"audio_stream\":" << source.audioStream << ",\"pictures\":[";
    bool first = true;
    const auto video = DecodePreviewVideo(source, target, control, [&](PreviewPicture picture) {
        RequirePreview(picture.generation == 7 && !picture.bgra.empty(), "Invalid decoded preview picture");
        if (!first) json << ',';
        first = false;
        json << "{\"pts_us\":" << picture.ptsUs << ",\"duration_us\":" << picture.durationUs
            << ",\"width\":" << picture.width << ",\"height\":" << picture.height << ",\"blue\":" << int(picture.bgra[0]) << '}';
    });
    json << "],\"video_frames\":" << video.frames << ",\"video_end_us\":" << video.endUs;
    int64_t samples = 0, firstSample = -1, previousEnd = -1;
    if (source.audioStream >= 0) {
        const auto audio = DecodePreviewAudio(source, target, control, [&](PreviewAudioBlock block) {
            RequirePreview(block.generation == 7 && block.frames > 0 && block.frames <= PreviewBlockFrames, "Invalid preview audio block");
            if (firstSample < 0) firstSample = block.startSample;
            RequirePreview(previousEnd < 0 || previousEnd == block.startSample, "PCM output timeline is discontinuous");
            previousEnd = block.startSample + block.frames;
            samples += block.frames;
            pcm.write(reinterpret_cast<const char*>(block.samples.data()), block.frames * PreviewChannels * sizeof(float));
            RequirePreview(bool(pcm), "Write inspected PCM failed");
        });
        json << ",\"audio_end_us\":" << audio.endUs;
    }
    json << ",\"audio_frames\":" << samples << ",\"first_sample\":" << firstSample << '}';
    json.close(); pcm.close();
    RequirePreview(bool(json) && bool(pcm), "Close preview inspection output failed");
    // 代次过期必须在碰不存在输入前就返回专用中断，不当成文件损坏。
    generation.store(8);
    bool interrupted = false;
    try { ReadPreviewSource(L"does-not-exist.nut", control); } catch (const PreviewInterrupted&) { interrupted = true; }
    RequirePreview(interrupted, "Superseded preview request was not rejected before input access");
    std::puts("PASS: actual preview decoder pixels/timestamps/PCM written for independent verification");
}

// 一次播放依次检查真实时钟推进、暂停冻结、跨GOP定位、旧代次拒绝及EOF后的重新定位。
void TestVideoPlayback(const std::wstring& file) {
    VideoPlayer player;
    player.SetVolume(0);
    player.Start(file);
    AwaitPreview([&] { CheckVideoFailure(player); return player.Snapshot().positionUs > 150000 && bool(player.TakePicture()); }, "Preview did not start");
    player.SetPaused(true);
    AwaitPreview([&] { CheckVideoFailure(player); return player.Snapshot().state == PlaybackState::Paused; }, "Preview pause was not acknowledged");
    const auto paused = player.Snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    RequirePreview(player.Snapshot().positionUs == paused.positionUs, "Paused master clock advanced");
    auto seekAndCheck = [&](double target) {
        const uint64_t gen = player.Seek(target);
        RequirePreview(!player.TakePicture(), "Seek left an old picture visible");
        std::shared_ptr<const PreviewPicture> picture;
        AwaitPreview([&] {
            CheckVideoFailure(player);
            picture = player.TakePicture();
            const auto state = player.Snapshot();
            return state.acknowledgedGeneration == gen && !state.seeking && picture && picture->generation == gen;
        }, "Paused seek did not publish target generation");
        const int64_t targetUs = static_cast<int64_t>(std::llround(target * 1000000));
        RequirePreview(picture->ptsUs <= targetUs && targetUs < picture->ptsUs + picture->durationUs, "Seek did not choose the picture covering target");
        RequirePreview(player.Snapshot().state == PlaybackState::Paused, "Paused seek unexpectedly resumed");
        std::printf("SEEK generation=%llu target=%lld picture=%lld duration=%lld\n", static_cast<unsigned long long>(gen),
            static_cast<long long>(targetUs), static_cast<long long>(picture->ptsUs), static_cast<long long>(picture->durationUs));
    };
    seekAndCheck(1.10);
    // 连续替换时只允许最后一个目标成为当前图；旧EOF、旧帧、旧读取均须失效。
    player.Seek(0.4); player.Seek(2.3); seekAndCheck(1.7);
    player.SetPaused(false);
    AwaitPreview([&] { CheckVideoFailure(player); player.TakePicture(); return player.Snapshot().positionUs > 1800000; }, "Preview resume did not advance");
    player.Stop(); JoinVideo(player);
    RequirePreview(player.Snapshot().state == PlaybackState::Stopped, "Preview stop failed");
    // 从结尾前播放到EOF，检查时钟/双轨尾部都能收完，再从自然结束状态跳回前面。
    const double duration = paused.durationUs.value_or(4000000) / 1000000.0;
    player.Start(file, {}, std::max(0.0, duration - 0.6));
    JoinVideo(player);
    const auto ended = player.Snapshot();
    RequirePreview(ended.state == PlaybackState::Completed, "Preview natural EOF failed");
    RequirePreview(ended.durationUs && ended.positionUs >= *ended.durationUs - 1000, "Preview completed before device clock covered media tail");
    RequirePreview(ended.videoQueueHighWater <= VideoPlayer::VideoCapacity && ended.audioQueueHighWater <= VideoPlayer::AudioCapacity, "Unbounded preview queues");
    player.Seek(0.6);
    AwaitPreview([&] { CheckVideoFailure(player); player.TakePicture(); return player.Snapshot().positionUs > 700000; }, "Seek after EOF failed");
    player.Stop(); JoinVideo(player);
    // 在打开输入时连续seek再停止，检查metadata读取被代次中断时会重试最新请求，而非结束新会话。
    player.Start(file); player.Seek(2.0); player.Seek(0.3); player.Stop(); JoinVideo(player);
    RequirePreview(player.Snapshot().state == PlaybackState::Stopped, "Seek/stop during opening failed");
    // 消费端发布回调失败时，也必须唤醒满队列生产者并真实回收。
    player.Start(file, [] { throw std::runtime_error("Intentional preview callback failure"); });
    AwaitPreview([&] { return player.TryJoin(); }, "Preview failure did not join workers");
    RequirePreview(player.Snapshot().state == PlaybackState::Failed, "Preview callback failure was hidden");
    std::printf("PASS video playback; audio_master=%d; scheduled=%lld; schedule_lateness_us=%lld; queues=%zu/%zu; position_us=%lld\n",
        ended.audioMaster, static_cast<long long>(ended.scheduledFrames), static_cast<long long>(ended.maxScheduleLatenessUs),
        ended.videoQueueHighWater, ended.audioQueueHighWater, static_cast<long long>(ended.positionUs));
}

// 在不跳转的完整播放中采集调度与媒体时钟，独立核对尾部、静音补齐和源帧覆盖。
void MeasureVideoPlayback(const std::wstring& file) {
    VideoPlayer player; player.SetVolume(0); player.Start(file);
    int64_t observations = 0, maxMailboxLate = 0;
    const auto started = std::chrono::steady_clock::now();
    AwaitPreview([&] {
        CheckVideoFailure(player);
        const auto picture = player.TakePicture();
        const auto status = player.Snapshot();
        if (picture && picture->generation == status.generation && !status.seeking) {
            ++observations;
            if (!picture->finalFrame) maxMailboxLate = std::max(maxMailboxLate,
                std::max<int64_t>(0, status.positionUs - picture->ptsUs - picture->durationUs));
        }
        return player.TryJoin();
    }, "Complete preview did not finish within deadline");
    const auto ended = player.Snapshot();
    RequirePreview(ended.state == PlaybackState::Completed, "Measured preview did not complete");
    RequirePreview(ended.durationUs && ended.positionUs >= *ended.durationUs - 1000, "Hardware clock did not cover submitted tail");
    RequirePreview(ended.videoQueueHighWater <= VideoPlayer::VideoCapacity && ended.audioQueueHighWater <= VideoPlayer::AudioCapacity, "Preview buffering exceeded capacity");
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("PREVIEW_MEASURE_BEGIN\n{\"audio_master\":%d,\"duration_us\":%lld,\"position_us\":%lld,\"prepared_frames\":%lld,"
        "\"scheduled_frames\":%lld,\"dropped_frames\":%lld,\"submitted_audio_frames\":%lld,\"silence_frames\":%lld,"
        "\"late_audio_frames\":%lld,\"video_queue\":%zu,\"audio_queue\":%zu,\"schedule_lateness_us\":%lld,"
        "\"mailbox_lateness_us\":%lld,\"observations\":%lld,\"starvation_intervals\":%llu,\"wall_seconds\":%.6f}\nPREVIEW_MEASURE_END\n",
        ended.audioMaster, static_cast<long long>(ended.durationUs.value_or(-1)), static_cast<long long>(ended.positionUs),
        static_cast<long long>(ended.decodedVideoFrames), static_cast<long long>(ended.scheduledFrames), static_cast<long long>(ended.droppedFrames),
        static_cast<long long>(ended.submittedAudioFrames), static_cast<long long>(ended.insertedSilenceFrames), static_cast<long long>(ended.lateAudioFrames),
        ended.videoQueueHighWater, ended.audioQueueHighWater, static_cast<long long>(ended.maxScheduleLatenessUs),
        static_cast<long long>(maxMailboxLate), static_cast<long long>(observations), static_cast<unsigned long long>(ended.starvationIntervals), wall);
}
