// S05在这里把带时间戳的画面、声音和Windows设备时钟接成一次播放。
// 两个生产线程分别读同一文件的选中轨道，Run只消费有限队列；这样视频队列满不会挡住音轨读包。
// 跳转先换generation使旧结果失效，Run再停止/清设备缓冲并重锚时钟，准备好新数据后才确认定位完成。
#define NOMINMAX
#include "VideoPlayer.h"
#include "../media/ffmpeg/PreviewInput.h"
#include "../media/video/VideoPreview.h"
#include "../media/audio/AudioPreview.h"
#include "../platform/windows/output/WasapiOutput.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace miniclip {
namespace {
// 所有外部秒数先校验再转成微秒，避免NaN、溢出或负数进入seek和时间戳计算。
int64_t Microseconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 86400)
        throw std::invalid_argument("Preview time must be finite and within 0..86400 seconds");
    return static_cast<int64_t>(std::llround(seconds * 1000000));
}
// PCM按整音频帧交付；向上取整可保证seek目标以前的样本不进入新设备缓冲。
int64_t SamplesAt(int64_t us) { return av_rescale_rnd(us, PreviewRate, 1000000, AV_ROUND_UP); }
// 已提交样本的位置换成统一媒体时间，不使用系统日历当播放时钟。
int64_t TimeAt(int64_t samples) { return av_rescale_q(samples, AVRational{1, PreviewRate}, AVRational{1, 1000000}); }
}

// 控制对象必须活到所有借用它的线程退出；异常离开同样请求停止并真实join。
VideoPlayer::~VideoPlayer() { Shutdown(); }

// 窗口和CLI先建立新请求，再启动Run；generation跨多次Start仍递增，旧窗口通知不能碰巧匹配新播放。
void VideoPlayer::Start(const std::wstring& file, std::function<void()> frameReady, double seconds, bool paused) {
    const int64_t target = Microseconds(seconds);
    if (worker_.joinable()) throw std::logic_error("Join previous video playback before restarting");
    {
        std::lock_guard lock(mutex_);
        stop_.store(false); noticePending_.store(false);
        const uint64_t gen = generation_.fetch_add(1) + 1;
        targetUs_ = target; paused_ = paused; lastFile_ = file; frameReady_ = std::move(frameReady);
        video_.clear(); audio_.clear(); picture_.reset(); failure_.clear();
        videoDone_ = audioDone_ = 0; videoEndUs_ = audioEndUs_ = target;
        snapshot_ = {}; snapshot_.state = PlaybackState::Opening; snapshot_.generation = gen;
        snapshot_.positionUs = snapshot_.requestedUs = target; snapshot_.seeking = true;
    }
    try { worker_ = std::thread(&VideoPlayer::Run, this, file); }
    catch (...) { std::lock_guard lock(mutex_); snapshot_.state = PlaybackState::Failed; throw; }
}

// 暂停按钮只发布意图；设备Stop与单调时钟冻结由拥有它们的Run执行。
void VideoPlayer::SetPaused(bool paused) {
    { std::lock_guard lock(mutex_); paused_ = paused; }
    changed_.notify_all();
}

// 当前播放立即换代并清显示/队列。已自然结束且回收后，重新打开同一文件定位，仍保留暂停意图。
uint64_t VideoPlayer::Seek(double seconds) {
    int64_t target = Microseconds(seconds);
    if (!worker_.joinable()) {
        std::wstring file; std::function<void()> callback; bool paused;
        {
            std::lock_guard lock(mutex_);
            if (snapshot_.state != PlaybackState::Completed) throw std::logic_error("Open a video before seeking");
            file = lastFile_; callback = frameReady_; paused = paused_;
            if (snapshot_.durationUs) target = std::min(target, std::max<int64_t>(0, *snapshot_.durationUs - 1));
        }
        Start(file, std::move(callback), target / 1000000.0, paused);
        return generation_.load();
    }
    uint64_t gen;
    {
        std::lock_guard lock(mutex_);
        if (stop_.load() || PlaybackFinished(snapshot_.state)) throw std::logic_error("Wait for video thread cleanup before seeking again");
        if (snapshot_.durationUs) target = std::min(target, std::max<int64_t>(0, *snapshot_.durationUs - 1));
        gen = generation_.fetch_add(1) + 1;
        targetUs_ = target; video_.clear(); audio_.clear(); picture_.reset();
        videoDone_ = audioDone_ = 0; videoEndUs_ = audioEndUs_ = target; failure_.clear();
        snapshot_.generation = gen; snapshot_.requestedUs = snapshot_.positionUs = target;
        snapshot_.seeking = true; snapshot_.state = PlaybackState::Opening;
        snapshot_.lastPictureUs = -1;
    }
    changed_.notify_all();
    return gen;
}

// 结束整次会话时唤醒入队/暂停/EOF等待；请求不等于线程已退出，界面仍须TryJoin。
void VideoPlayer::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_.store(true);
        if (snapshot_.state != PlaybackState::Idle && !PlaybackFinished(snapshot_.state)) snapshot_.state = PlaybackState::Stopping;
    }
    changed_.notify_all();
}
// 增益仅影响本次会话随后提交的PCM，不修改系统音量或源文件。
void VideoPlayer::SetVolume(float volume) {
    if (!std::isfinite(volume) || volume < 0 || volume > 1) throw std::invalid_argument("Video volume must be 0..1");
    volume_.store(volume);
}
// 控制线程拿到自有副本，不能直接并发读取工作队列或设备接口。
VideoPlaybackSnapshot VideoPlayer::Snapshot() const { std::lock_guard lock(mutex_); return snapshot_; }
// GUI确认合并通知后取当前代次的画面，智能指针让像素在这次绘制期间保持有效。
std::shared_ptr<const PreviewPicture> VideoPlayer::TakePicture() {
    std::lock_guard lock(mutex_); noticePending_.store(false);
    return picture_ && picture_->generation == generation_.load() ? picture_ : nullptr;
}
// 绘制成功后记录“真的执行了WM_PAINT”的观测；过期代次或重复绘制不冒充新画面。
void VideoPlayer::ReportPainted(uint64_t gen, int64_t ptsUs, int64_t durationUs) {
    std::lock_guard lock(mutex_);
    if (gen != generation_.load() || !picture_ || picture_->ptsUs != ptsUs) return;
    ++snapshot_.paintedFrames;
    if (!picture_->finalFrame) snapshot_.maxPaintLatenessUs = std::max(snapshot_.maxPaintLatenessUs,
        std::max<int64_t>(0, snapshot_.positionUs - ptsUs - durationUs));
}
// Windows零等待观察真实线程退出，再join；普通轮询不占住GUI等完整视频。
bool VideoPlayer::TryJoin() {
    if (!worker_.joinable()) return false;
    const DWORD state = WaitForSingleObject(worker_.native_handle(), 0);
    if (state == WAIT_TIMEOUT) return false;
    if (state != WAIT_OBJECT_0) throw std::runtime_error("Cannot inspect video playback thread");
    worker_.join(); return true;
}
// 入口异常/析构兜底仍真实等待，不用detach或超时假装资源已经安全释放。
void VideoPlayer::Shutdown() { Stop(); if (worker_.joinable()) worker_.join(); }

// 视频线程每代独立打开并seek，旧调用的FFmpeg/sws资源在作用域结束时归还。
void VideoPlayer::ProduceVideo(PreviewSource source) {
    while (!stop_.load()) {
        uint64_t gen; int64_t target;
        { std::lock_guard lock(mutex_); gen = generation_.load(); target = targetUs_; }
        try {
            const auto result = DecodePreviewVideo(source, target, {stop_, generation_, gen}, [&](PreviewPicture value) {
                auto picture = std::make_shared<const PreviewPicture>(std::move(value));
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stop_.load() || generation_.load() != gen || video_.size() < VideoCapacity; });
                if (stop_.load() || generation_.load() != gen) throw PreviewInterrupted();
                videoEndUs_ = std::max(videoEndUs_, picture->ptsUs + picture->durationUs);
                video_.push_back(std::move(picture)); ++snapshot_.decodedVideoFrames;
                snapshot_.videoQueueHighWater = std::max(snapshot_.videoQueueHighWater, video_.size());
                lock.unlock(); changed_.notify_all();
            });
            std::lock_guard lock(mutex_);
            if (gen == generation_.load()) { videoDone_ = gen; videoEndUs_ = std::max(videoEndUs_, result.endUs); }
        } catch (const PreviewInterrupted&) {
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            if (gen == generation_.load() && !stop_.load()) failure_ = error.what();
        } catch (...) { std::lock_guard lock(mutex_); if (gen == generation_.load() && !stop_.load()) failure_ = "Unknown video decoder failure"; }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stop_.load() || generation_.load() != gen || !failure_.empty(); });
        if (!failure_.empty()) return;
    }
}
// 声音线程与视频分别读文件，保留源样本位置；暂停也只提前准备有限8块，seek能够打破所有等待。
void VideoPlayer::ProduceAudio(PreviewSource source) {
    while (!stop_.load()) {
        uint64_t gen; int64_t target;
        { std::lock_guard lock(mutex_); gen = generation_.load(); target = targetUs_; }
        try {
            const auto result = DecodePreviewAudio(source, target, {stop_, generation_, gen}, [&](PreviewAudioBlock block) {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stop_.load() || generation_.load() != gen || audio_.size() < AudioCapacity; });
                if (stop_.load() || generation_.load() != gen) throw PreviewInterrupted();
                audioEndUs_ = std::max(audioEndUs_, TimeAt(block.startSample + block.frames));
                audio_.push_back(std::move(block));
                snapshot_.audioQueueHighWater = std::max(snapshot_.audioQueueHighWater, audio_.size());
                lock.unlock(); changed_.notify_all();
            });
            std::lock_guard lock(mutex_);
            if (gen == generation_.load()) { audioDone_ = gen; audioEndUs_ = std::max(audioEndUs_, result.endUs); }
        } catch (const PreviewInterrupted&) {
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            if (gen == generation_.load() && !stop_.load()) failure_ = error.what();
        } catch (...) { std::lock_guard lock(mutex_); if (gen == generation_.load() && !stop_.load()) failure_ = "Unknown audio decoder failure"; }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stop_.load() || generation_.load() != gen || !failure_.empty(); });
        if (!failure_.empty()) return;
    }
}

// 只发布当前代次最新画面，并最多挂一份GUI通知；像素放邮箱，消息不携带裸指针。
void VideoPlayer::Publish(std::shared_ptr<const PreviewPicture> picture, int64_t clockUs) {
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (picture->generation != generation_.load() || stop_.load()) return;
        picture_ = std::move(picture); snapshot_.lastPictureUs = picture_->ptsUs; ++snapshot_.scheduledFrames;
        if (!picture_->finalFrame) snapshot_.maxScheduleLatenessUs = std::max(snapshot_.maxScheduleLatenessUs,
            std::max<int64_t>(0, clockUs - picture_->ptsUs - picture_->durationUs));
        notify = !noticePending_.exchange(true);
    }
    if (notify && frameReady_) frameReady_();
}

// Run拥有唯一WASAPI会话。带声音时用IAudioClock调画面，无声音时才用可暂停的steady_clock。
// 每代先清设备再准备数据；自然结束等两轨、队列和设备呈现时钟，任何失败都唤醒并join两生产者。
void VideoPlayer::Run(std::wstring file) {
    std::thread videoProducer, audioProducer;
    PlaybackState finalState = PlaybackState::Completed;
    std::string error;
    try {
        PreviewSource source;
        for (;;) {
            const uint64_t opening = generation_.load();
            try { source = ReadPreviewSource(file, {stop_, generation_, opening}); break; }
            catch (const PreviewInterrupted&) { if (stop_.load()) throw; }
        }
        {
            std::lock_guard lock(mutex_);
            snapshot_.durationUs = source.durationUs; snapshot_.audioMaster = source.audioStream >= 0;
            if (source.durationUs) targetUs_ = std::min(targetUs_, std::max<int64_t>(0, *source.durationUs - 1));
        }
        std::unique_ptr<WasapiOutput> output;
        if (source.audioStream >= 0) { output = std::make_unique<WasapiOutput>(); output->Open(); output->EnableClock(); }
        videoProducer = std::thread(&VideoPlayer::ProduceVideo, this, source);
        if (output) audioProducer = std::thread(&VideoPlayer::ProduceAudio, this, source);
        uint64_t gen = 0;
        int64_t baseUs = 0, clockUs = 0, submittedSample = 0;
        bool running = false, ready = false;
        auto wallStarted = std::chrono::steady_clock::now();
        auto lastProgress = wallStarted;
        int64_t lastClock = -1;
        PreviewAudioBlock current;
        int offset = 0;
        std::array<float, PreviewBlockFrames * PreviewChannels> silence{};
        for (;;) {
            if (stop_.load()) throw PreviewInterrupted();
            if (generation_.load() != gen) {
                // 旧Write至此已返回；同一线程Stop/Reset后才允许新代次向设备写入。
                if (output) output->ResetStopped();
                std::lock_guard lock(mutex_);
                gen = generation_.load(); clockUs = baseUs = targetUs_; submittedSample = SamplesAt(baseUs);
                current = {}; offset = 0; running = ready = false; lastClock = -1;
                wallStarted = lastProgress = std::chrono::steady_clock::now();
            }
            bool paused, audioDone, videoDone; int64_t endUs;
            {
                std::unique_lock lock(mutex_);
                if (!failure_.empty()) throw std::runtime_error(failure_);
                if (generation_.load() != gen) continue;
                paused = paused_; audioDone = !output || audioDone_ == gen; videoDone = videoDone_ == gen;
                endUs = std::max(videoEndUs_, audioEndUs_);
                if (audioDone && videoDone && endUs > 0) snapshot_.durationUs = endUs;
                if (!ready) {
                    const bool videoReady = !video_.empty() || videoDone;
                    const bool audioReady = !output || !audio_.empty() || audioDone;
                    if (!videoReady || !audioReady) {
                        changed_.wait_for(lock, std::chrono::milliseconds(10));
                        continue;
                    }
                    ready = true; snapshot_.acknowledgedGeneration = gen; snapshot_.seeking = false;
                }
            }
            if (output) {
                if (paused && running) { output->Pause(); running = false; }
                clockUs = std::min(baseUs + static_cast<int64_t>(std::llround(output->ClockSeconds() * 1000000)), TimeAt(submittedSample));
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (running) clockUs = baseUs + std::chrono::duration_cast<std::chrono::microseconds>(now - wallStarted).count();
                if (paused && running) { baseUs = clockUs; running = false; }
                if (!paused && !running) { wallStarted = now; baseUs = clockUs; running = true; }
            }
            if (generation_.load() != gen) continue;
            std::shared_ptr<const PreviewPicture> due;
            {
                std::lock_guard lock(mutex_);
                if (generation_.load() != gen) continue;
                snapshot_.positionUs = clockUs; snapshot_.state = paused ? PlaybackState::Paused : PlaybackState::Playing;
                while (!video_.empty() && video_.front()->ptsUs <= clockUs) {
                    if (due) ++snapshot_.droppedFrames;
                    due = std::move(video_.front()); video_.pop_front();
                }
                changed_.notify_all();
            }
            if (due) Publish(std::move(due), clockUs);
            if (paused) {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stop_.load() || generation_.load() != gen || !paused_ || !failure_.empty(); });
                lastProgress = std::chrono::steady_clock::now();
                continue;
            }
            if (output) {
                UINT32 padding = output->Padding();
                {
                    std::lock_guard lock(mutex_);
                    audioDone = audioDone_ == gen; videoDone = videoDone_ == gen;
                    endUs = std::max(videoEndUs_, audioEndUs_);
                }
                // 真正断粮时先冻结/清设备再以已提交内容末尾重锚，不能把设备空转当成源内容已经播放。
                // padding归零后声音可能仍在设备管线中。必须等IAudioClock覆盖已提交内容，才能重缓冲或结束。
                if (running && padding == 0 && clockUs >= TimeAt(submittedSample) - 1000
                    && !(audioDone && videoDone && clockUs >= endUs - 1000)) {
                    output->ResetStopped(); running = false; baseUs = clockUs = TimeAt(submittedSample);
                    std::lock_guard lock(mutex_); ++snapshot_.starvationIntervals;
                }
                UINT32 available = output->Capacity() - padding;
                while (available && generation_.load() == gen && !stop_.load()) {
                    {
                        std::lock_guard lock(mutex_);
                        if (offset == current.frames && !audio_.empty()) {
                            current = std::move(audio_.front()); audio_.pop_front(); offset = 0; changed_.notify_all();
                        }
                        audioDone = audioDone_ == gen; videoDone = videoDone_ == gen;
                        endUs = std::max(videoEndUs_, audioEndUs_);
                    }
                    if (offset < current.frames && current.generation != gen) { current = {}; offset = 0; continue; }
                    const int64_t next = current.startSample + offset;
                    if (offset < current.frames && next < submittedSample) {
                        const int skip = static_cast<int>(std::min<int64_t>(current.frames - offset, submittedSample - next));
                        offset += skip; std::lock_guard lock(mutex_); snapshot_.lateAudioFrames += skip; continue;
                    }
                    bool zero = offset == current.frames || next > submittedSample;
                    int64_t count = std::min<UINT32>(available, PreviewBlockFrames);
                    if (offset < current.frames) count = std::min<int64_t>(count, zero ? next - submittedSample : current.frames - offset);
                    else if (!audioDone) break; // 暂时缺数据先等生产者，不无限填静音掩盖解码迟到。
                    // 已知声音结束后，只补到已经准备好的画面末尾，不能提前多写一整个设备缓冲的静音。
                    if (audioDone) count = std::min<int64_t>(count, std::max<int64_t>(0, SamplesAt(endUs) - submittedSample));
                    if (!count) break;
                    if (generation_.load() != gen) break;
                    output->Write(zero ? silence.data() : current.samples.data() + offset * PreviewChannels,
                        static_cast<UINT32>(count), volume_.load());
                    submittedSample += count; available -= static_cast<UINT32>(count);
                    if (!zero) offset += static_cast<int>(count);
                    {
                        std::lock_guard lock(mutex_);
                        if (generation_.load() == gen) { snapshot_.submittedAudioFrames += count; if (zero) snapshot_.insertedSilenceFrames += count; }
                    }
                }
                if (!running && output->Padding() > 0 && generation_.load() == gen && !stop_.load()) { output->Start(); running = true; }
            }
            bool complete;
            {
                std::lock_guard lock(mutex_);
                complete = generation_.load() == gen && videoDone_ == gen && (!output || audioDone_ == gen)
                    && video_.empty() && (!output || (audio_.empty() && offset == current.frames))
                    && clockUs >= std::max(videoEndUs_, audioEndUs_) - 1000;
            }
            if (complete && (!output || output->Padding() == 0)) {
                std::lock_guard lock(mutex_);
                if (gen == generation_.load() && !stop_.load()) {
                    snapshot_.state = PlaybackState::Completed; snapshot_.positionUs = clockUs;
                    snapshot_.durationUs = std::max(videoEndUs_, audioEndUs_);
                    stop_.store(true); break;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (clockUs != lastClock) { lastClock = clockUs; lastProgress = now; }
            if (now - lastProgress > std::chrono::seconds(10)) throw std::runtime_error("Preview clock made no progress for 10 seconds");
            if (output && running) output->Wait();
            else { std::unique_lock lock(mutex_); changed_.wait_for(lock, std::chrono::milliseconds(5)); }
        }
        if (output) output->Pause();
    } catch (const PreviewInterrupted&) { finalState = PlaybackState::Stopped; }
    catch (const std::exception& failure) { finalState = PlaybackState::Failed; error = failure.what(); }
    catch (...) { finalState = PlaybackState::Failed; error = "Unknown video playback failure"; }
    // 同一mutex内置停止再通知，生产者即使正准备休眠也不会漏掉这次结束请求。
    { std::lock_guard lock(mutex_); stop_.store(true); }
    changed_.notify_all();
    if (videoProducer.joinable()) videoProducer.join();
    if (audioProducer.joinable()) audioProducer.join();
    {
        std::lock_guard lock(mutex_);
        if (!failure_.empty()) { finalState = PlaybackState::Failed; error = failure_; }
        video_.clear(); audio_.clear(); snapshot_.state = finalState; snapshot_.seeking = false; snapshot_.message = error;
    }
}
}
