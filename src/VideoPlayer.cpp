#include "VideoPlayer.h"
#include "platform/Platform.h"
#include "platform/PlatformMedia.h"
#include "FFmpegReader.h"

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() {
    Close();
}

bool VideoPlayer::Load(const std::wstring& filePath, std::shared_ptr<VideoPlayer> previousPlayer) {
    Close();
    if (filePath.empty()) {
        if (previousPlayer) ClosePrevious(std::move(previousPlayer));
        return false;
    }

    currentPath_ = filePath;
    stopThread_.store(false);
    isPlaying_.store(false);
    isLoaded_.store(false);
    error_.store(S_OK);
    audioError_.store(Platform::kAudioErrorNone); lastAudioTime100ns_.store(-1);
    seeking_.store(false);
    lastExactTarget100ns_.store(-1);
    sharedStartNs_.store(0);
    atEof_.store(false);
    pendingSeek100ns_.store(-1);
    currentPos100ns_.store(0);
    duration100ns_.store(0);
    videoWidth_.store(0);
    videoHeight_.store(0);

    workerThread_ = std::thread(&VideoPlayer::WorkerLoop, this, filePath, std::move(previousPlayer));
    return true;
}

void VideoPlayer::ClosePrevious(std::shared_ptr<VideoPlayer> previousPlayer) {
    Close();
    if (!previousPlayer) return;
    stopThread_.store(false);
    workerThread_ = std::thread([prev = std::move(previousPlayer)]() mutable {
        if (prev) {
            prev->Close();
            prev.reset();
        }
    });
}

void VideoPlayer::RequestStop() {
    stopThread_.store(true);
    isPlaying_.store(false);
    cv_.notify_all();
}

void VideoPlayer::Close() {
    RequestStop();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    isLoaded_.store(false);
    currentPath_.clear();
    currentPos100ns_.store(0);
    duration100ns_.store(0);
    lastExactTarget100ns_.store(-1);

    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        frame_.reset();
    }
}

void VideoPlayer::Play() {
    sharedStartNs_.store(0);
    lastExactTarget100ns_.store(-1);
    if (!isLoaded_.load()) return;
    // If at end of stream, restart from 0
    int64_t dur = duration100ns_.load();
    int64_t pos = currentPos100ns_.load();
    if (atEof_.load() || (dur > 0 && pos >= dur - 1500000LL)) {
        atEof_.store(false);
        SeekTo(0);
    }
    isPlaying_.store(true);
    clockRevision_.fetch_add(1);
    cv_.notify_all();
}

void VideoPlayer::PlayAt(std::chrono::steady_clock::time_point start, int64_t mediaTime100ns) {
    if (!isLoaded_.load()) return;
    lastExactTarget100ns_.store(-1);
    sharedMedia100ns_.store(mediaTime100ns);
    sharedStartNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count());
    isPlaying_.store(true);
    clockRevision_.fetch_add(1);
    cv_.notify_all();
}

void VideoPlayer::Pause() {
    isPlaying_.store(false);
    cv_.notify_all();
}

void VideoPlayer::TogglePlayPause() {
    if (isPlaying_.load()) {
        Pause();
    } else {
        Play();
    }
}

void VideoPlayer::Stop() {
    isPlaying_.store(false);
    SeekTo(0);
}

void VideoPlayer::SeekTo(int64_t pos100ns) {
    sharedStartNs_.store(0);
    int64_t dur = duration100ns_.load();
    if (pos100ns < 0) pos100ns = 0;
    if (dur > 0 && pos100ns > dur) pos100ns = dur;
    {
        std::lock_guard lock(stateMutex_);
        if (!isPlaying_.load() && !seeking_.load() && pendingSeek100ns_.load() < 0 && lastExactTarget100ns_.load() == pos100ns) {
            return;
        }
        seekPreviousFrame_ = false;
        seekExact_ = true;
        seeking_.store(true);
        pendingSeek100ns_.store(pos100ns);
    }
    cv_.notify_all();
}

void VideoPlayer::ScrubTo(int64_t pos100ns) {
    sharedStartNs_.store(0);
    int64_t dur = duration100ns_.load();
    if (pos100ns < 0) pos100ns = 0;
    if (dur > 0 && pos100ns > dur) pos100ns = dur;
    {
        std::lock_guard lock(stateMutex_);
        if (!isPlaying_.load() && !seeking_.load() && pendingSeek100ns_.load() < 0 && lastExactTarget100ns_.load() == pos100ns) {
            return;
        }
        if (lastExactTarget100ns_.load() != pos100ns) {
            lastExactTarget100ns_.store(-1);
        }
        seekPreviousFrame_ = false;
        seekExact_ = false;
        seeking_.store(true);
        pendingSeek100ns_.store(pos100ns);
    }
    cv_.notify_all();
}

void VideoPlayer::StepFrame(bool forward) {
    if(!IsLoaded()) return;
    Pause(); sharedStartNs_.store(0);
    {
        std::lock_guard lock(stateMutex_);
        lastExactTarget100ns_.store(-1);
        seekPreviousFrame_=!forward;
        seekExact_=true;
        seeking_.store(true);
        pendingSeek100ns_.store(currentPos100ns_.load()+(forward?1:0));
    }
    cv_.notify_all();
}

void VideoPlayer::SeekRatio(double ratio0to1) {
    if (ratio0to1 < 0.0) ratio0to1 = 0.0;
    if (ratio0to1 > 1.0) ratio0to1 = 1.0;
    int64_t dur = duration100ns_.load();
    if (dur > 0) {
        SeekTo(static_cast<int64_t>(dur * ratio0to1));
    }
}

void VideoPlayer::ScrubRatio(double ratio0to1) {
    if (ratio0to1 < 0.0) ratio0to1 = 0.0;
    if (ratio0to1 > 1.0) ratio0to1 = 1.0;
    int64_t dur = duration100ns_.load();
    if (dur > 0) {
        ScrubTo(static_cast<int64_t>(dur * ratio0to1));
    }
}

void VideoPlayer::SetMuted(bool muted) {
    isMuted_.store(muted);
    cv_.notify_all();
}

std::shared_ptr<const VideoFrame> VideoPlayer::GetFrame() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    return frame_;
}

void VideoPlayer::WorkerLoop(std::wstring path, std::shared_ptr<VideoPlayer> previousPlayer) {
    if (previousPlayer) {
        previousPlayer->Close();
        previousPlayer.reset();
    }
    if (stopThread_.load() || path.empty()) {
        return;
    }
    Platform::ComInit com(Platform::ComApartment::MultiThreaded);

    if(FFmpegReader::GpuEnabled()) { FFmpegLoop(path); return; }

    Platform::NativeVideoDecoder decoder;
    int64_t openedDuration = 0;
    uint32_t width = 0, height = 0;
    Platform::NativeAudioFormat audioFormat;
    const auto openStatus = decoder.Open(path, openedDuration, width, height, audioFormat);
    if (openedDuration != 0) duration100ns_.store(openedDuration);
    if (openStatus == Platform::NativeOpenStatus::FallbackToFFmpeg) {
        FFmpegLoop(path);
        return;
    }
    if (openStatus == Platform::NativeOpenStatus::InvalidDimensions) {
        return;
    }

    videoWidth_.store(width);
    videoHeight_.store(height);

    Platform::AudioOutput audioRing;
    bool hasAudio = false;
    if (audioFormat.available) {
        hasAudio = audioRing.Open(audioFormat.sampleRate, audioFormat.channels, audioFormat.bitsPerSample);
        audioError_.store(audioRing.error());
    }

    if (openStatus == Platform::NativeOpenStatus::InvalidMediaType) {
        error_.store(Platform::kErrorInvalidMediaType);
        return;
    }

    bool readerAtEof = false;
    int64_t eofFramePts100ns = -1;
    int64_t decoderPos100ns = -1;

    auto decodeSampleToFrameBuffer = [&](const Platform::NativeVideoSample& sample, uint32_t& outW, uint32_t& outH) {
        auto frame = std::make_shared<VideoFrame>();
        if (!decoder.DecodeVideoFrame(sample, frame->width, frame->height, frame->pixels)) return false;
        outW = frame->width;
        outH = frame->height;
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame_ = std::move(frame);
        return true;
    };

    auto publish = [&](const Platform::NativeVideoSample& sample, int64_t timestamp) {
        uint32_t outW = 0, outH = 0;
        if (!decodeSampleToFrameBuffer(sample, outW, outH)) return false;
        videoWidth_.store(outW);
        videoHeight_.store(outH);
        currentPos100ns_.store(timestamp);
        return true;
    };
    auto decodeSingleVideoFrame = [&](int64_t target = 0, bool previousFrame = false, bool exact = true) -> bool {
        Platform::NativeVideoSample previous;
        int64_t previousTime = 0;
        int64_t beforeSeekPos = currentPos100ns_.load();
        bool advancedPastBefore = false;
        auto scrubStart = std::chrono::steady_clock::now();
        for (int tries = 0; tries < 100000 && !stopThread_.load(); ++tries) {
            if (advancedPastBefore && pendingSeek100ns_.load(std::memory_order_relaxed) >= 0) {
                bool coalescedInPlace = false;
                {
                    std::lock_guard lock(stateMutex_);
                    int64_t cand = pendingSeek100ns_.load();
                    if (cand >= 0 && !previousFrame && !seekPreviousFrame_ && decoderPos100ns >= 0 &&
                        cand >= decoderPos100ns && (cand <= target || (cand - decoderPos100ns) <= 30000000LL)) {
                        target = pendingSeek100ns_.exchange(-1);
                        exact = seekExact_;
                        coalescedInPlace = true;
                    }
                }
                if (coalescedInPlace) {
                    if (previous && previousTime >= target) {
                        bool ok = publish(previous, previousTime);
                        if (ok) lastExactTarget100ns_.store(target);
                        return ok;
                    }
                } else {
                    if (previous && previousTime != beforeSeekPos &&
                        std::abs(target - previousTime) < std::abs(target - beforeSeekPos)) {
                        publish(previous, previousTime);
                    }
                    return true;
                }
            }
            auto step = decoder.ReadNextSample(true, false);
            if (step.streamError) {
                error_.store(FAILED(step.hr) ? step.hr : E_FAIL); return false;
            }
            if (step.mediaTypeInvalid) {
                error_.store(Platform::kErrorInvalidMediaType); return false;
            }
            if (step.endOfStream) {
                readerAtEof = true;
                decoderPos100ns = -1;
                if (previous) {
                    bool ok = publish(previous, previousTime);
                    if (ok) {
                        eofFramePts100ns = previousTime;
                        if (!previousFrame) lastExactTarget100ns_.store(target);
                    }
                    return ok;
                }
                if (GetFrame() != nullptr) {
                    eofFramePts100ns = currentPos100ns_.load();
                    if (!previousFrame) lastExactTarget100ns_.store(target);
                    return true;
                }
                return false;
            }
            if (!step.sample) continue;
            const int64_t timestamp = step.timestamp100ns;
            decoderPos100ns = timestamp;
            if (timestamp != beforeSeekPos) advancedPastBefore = true;
            if (!previousFrame && pendingSeek100ns_.load(std::memory_order_relaxed) >= 0) {
                std::lock_guard lock(stateMutex_);
                int64_t cand = pendingSeek100ns_.load();
                if (cand >= 0 && !seekPreviousFrame_ &&
                    cand >= timestamp && (cand <= target || (cand - timestamp) <= 30000000LL)) {
                    target = pendingSeek100ns_.exchange(-1);
                    exact = seekExact_;
                }
            }
            if (timestamp >= target) {
                if (previousFrame && previous) return publish(previous, previousTime);
                bool ok = publish(step.sample, timestamp);
                if (ok && !previousFrame) lastExactTarget100ns_.store(target);
                return ok;
            }
            previous = step.sample;
            previousTime = timestamp;
            if (!exact && !previousFrame) {
                bool closerToTarget = std::abs(target - timestamp) < std::abs(target - beforeSeekPos);
                if (advancedPastBefore && pendingSeek100ns_.load(std::memory_order_relaxed) >= 0) {
                    if (closerToTarget) publish(step.sample, timestamp);
                    return true;
                }
                auto now = std::chrono::steady_clock::now();
                auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - scrubStart).count();
                if (advancedPastBefore && closerToTarget && (std::abs(timestamp - beforeSeekPos) > 1000000 || elapsedMs >= 25)) {
                    publish(step.sample, timestamp);
                    beforeSeekPos = timestamp;
                    scrubStart = now;
                }
            }
        }
        return false;
    };

    if (!decodeSingleVideoFrame()) {
        audioRing.Close(); decoder.Close(); FFmpegLoop(path); return;
    }
    isLoaded_.store(true);
    bool wasPlaying = false;
    bool lastSeekWasScrub = false;
    uint64_t clockRevision=clockRevision_.load();
    bool wasMuted   = isMuted_.load();
    bool audioEof   = false;
    auto clockAnchor = std::chrono::steady_clock::now();
    int64_t mediaAnchor100ns = currentPos100ns_.load();
    int64_t audioFloor100ns=mediaAnchor100ns;
    auto canSubmit=[&] { return !stopThread_.load() && isPlaying_.load() && !isMuted_.load() && pendingSeek100ns_.load()<0; };

    while (!stopThread_.load(std::memory_order_relaxed)) {
        // 1. Handle any pending seek request
        int64_t seekTarget=-1; bool previousFrame=false; bool exactSeek=true;
        {
            std::lock_guard lock(stateMutex_);
            seekTarget=pendingSeek100ns_.exchange(-1); previousFrame=seekPreviousFrame_; exactSeek=seekExact_;
        }
        if (seekTarget >= 0) {
            audioFloor100ns=seekTarget; lastAudioTime100ns_.store(-1);
            atEof_.store(false);
            audioEof = false;
            audioRing.Reset();
            int64_t curPos = currentPos100ns_.load();
            bool alreadyAtTarget = !previousFrame && !wasPlaying &&
                                   ((lastExactTarget100ns_.load() == seekTarget) ||
                                    (eofFramePts100ns >= 0 && curPos == eofFramePts100ns && seekTarget >= eofFramePts100ns));
            if (alreadyAtTarget) {
                lastExactTarget100ns_.store(seekTarget);
            } else {
                bool canStepForward = lastSeekWasScrub && !previousFrame && !wasPlaying && !readerAtEof &&
                                      decoderPos100ns >= 0 && seekTarget > decoderPos100ns &&
                                      (seekTarget - decoderPos100ns) <= 30000000LL;
                HRESULT seekResult = S_OK;
                if (!canStepForward) {
                    readerAtEof = false;
                    decoderPos100ns = -1;
                    seekResult = decoder.FlushAndSeek(std::max<int64_t>(0,seekTarget-(previousFrame?1:0)));
                }

                if (FAILED(seekResult)) error_.store(seekResult);
                else if (!decodeSingleVideoFrame(seekTarget,previousFrame,exactSeek) && !stopThread_.load()) error_.store(E_FAIL);
            }
            lastSeekWasScrub = !exactSeek && !previousFrame;
            {
                std::lock_guard lock(stateMutex_);
                seeking_.store(pendingSeek100ns_.load()>=0);
            }

            clockAnchor = std::chrono::steady_clock::now();
            mediaAnchor100ns = currentPos100ns_.load();
            wasPlaying=false;
            continue;
        }

        // 2. Check mute transition
        bool curMuted = isMuted_.load(std::memory_order_relaxed);
        if (curMuted && !wasMuted) {
            audioRing.Reset();
        }
        if (!curMuted && wasMuted) audioFloor100ns = currentPos100ns_.load();
        wasMuted = curMuted;

        // 3. If paused, wait for Play or Seek or Close
        if (!isPlaying_.load(std::memory_order_relaxed)) {
            if (wasPlaying) {
                audioRing.Reset();
                wasPlaying = false;
            }
            std::unique_lock<std::mutex> lk(stateMutex_);
            cv_.wait_for(lk, std::chrono::milliseconds(30), [&]() {
                return stopThread_.load() || isPlaying_.load() || (pendingSeek100ns_.load() >= 0);
            });
            continue;
        }

        if (!wasPlaying || clockRevision!=clockRevision_.load()) {
            clockRevision=clockRevision_.load();
            wasPlaying = true;
            clockAnchor = std::chrono::steady_clock::now();
            mediaAnchor100ns = currentPos100ns_.load();
            if (int64_t shared = sharedStartNs_.load(); shared > 0) {
                clockAnchor = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(shared));
                mediaAnchor100ns = sharedMedia100ns_.load();
            }
            audioFloor100ns=mediaAnchor100ns;
        }

        // 4. Playing: Read next sample from active stream(s)
        auto step = decoder.ReadNextSample(false, hasAudio && !audioEof);
        if (FAILED(step.hr) || step.endOfStream) {
            if (!FAILED(step.hr) && hasAudio && !step.isVideoStream) {
                // Only the audio stream ended early; continue reading video frames!
                audioEof = true;
                continue;
            }
            readerAtEof = true;
            decoderPos100ns = -1;
            atEof_.store(true);
            if (duration100ns_.load() > 0) {
                currentPos100ns_.store(duration100ns_.load());
            }
            isPlaying_.store(false);
            audioRing.Reset();
            wasPlaying = false;
            continue;
        }

        if (step.mediaTypeInvalid && step.isVideoStream) {
            error_.store(Platform::kErrorInvalidMediaType); isPlaying_.store(false); continue;
        }
        if (!step.sample) continue;

        const int64_t llTimeStamp = step.timestamp100ns;
        if (step.isVideoStream) {
            decoderPos100ns = llTimeStamp;
            if(llTimeStamp<0 || llTimeStamp>INT64_MAX/100 || mediaAnchor100ns<0 || mediaAnchor100ns>INT64_MAX/100) {
                error_.store(Platform::kErrorInvalidTimestamp); isPlaying_.store(false); continue;
            }
            // Keep the original clock anchor; elapsed playback is not a timestamp discontinuity.
            int64_t delta100ns = llTimeStamp - mediaAnchor100ns;

            auto targetTime = clockAnchor + std::chrono::nanoseconds(delta100ns * 100);
            while (!stopThread_.load() && isPlaying_.load() && pendingSeek100ns_.load() < 0) {
                auto now = std::chrono::steady_clock::now();
                if (now >= targetTime) break;
                auto remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(targetTime - now).count();
                if (remainMs > 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(std::min<long long>(remainMs - 1, 8)));
                } else {
                    std::this_thread::yield();
                }
            }

            if (stopThread_.load() || !isPlaying_.load() || pendingSeek100ns_.load() >= 0) {
                continue;
            }

            uint32_t outW = 0, outH = 0;
            if (decodeSampleToFrameBuffer(step.sample, outW, outH)) {
                videoWidth_.store(outW);
                videoHeight_.store(outH);
                currentPos100ns_.store(llTimeStamp);
                lastExactTarget100ns_.store(-1);
            }
        } else if (hasAudio && !isMuted_.load(std::memory_order_relaxed)) {
            decoder.WithAudioPcm(step.sample, [&](const uint8_t* pAudioBytes, uint32_t cbCur) {
                uint64_t skip=0;
                int64_t timestamp=llTimeStamp;
                if(timestamp<audioFloor100ns) {
                    uint64_t frames=static_cast<uint64_t>(audioFloor100ns-timestamp)*audioRing.sampleRate()/10000000;
                    skip=frames*audioRing.blockAlign(); timestamp=audioFloor100ns;
                }
                if(skip<cbCur) {
                    // Keep PCM near the shared clock; never play keyframe preroll after a seek.
                    int64_t delta=timestamp-mediaAnchor100ns;
                    if(delta>=0 && delta<=INT64_MAX/100) {
                        auto due=clockAnchor+std::chrono::nanoseconds(delta*100)-std::chrono::milliseconds(40);
                        while(canSubmit() && std::chrono::steady_clock::now()<due) Platform::SleepShortMs(1);
                        if(audioRing.WritePCM(pAudioBytes+skip,cbCur-static_cast<uint32_t>(skip),canSubmit)) lastAudioTime100ns_.store(timestamp);
                        audioError_.store(audioRing.error());
                    }
                }
            });
        }
    }

    audioRing.Close();
    decoder.Close();
}

void VideoPlayer::FFmpegLoop(const std::wstring& path) {
    FFmpegReader reader; VideoFileInfo metadata; FFmpegReader::Sample sample;
    if(!reader.Open(path,&stopThread_,true) || !reader.Metadata(metadata) || !reader.VideoAt(0,sample)) { if(!stopThread_.load()) error_=Platform::kErrorInvalidMediaType; return; }
    duration100ns_=metadata.duration100ns;
    auto publish=[&](FFmpegReader::Sample& value) {
        auto frame=std::make_shared<VideoFrame>(); frame->width=value.width; frame->height=value.height; frame->pixels=std::move(value.bytes);
        { std::lock_guard lock(frameMutex_); frame_=std::move(frame); }
        videoWidth_=value.width; videoHeight_=value.height; currentPos100ns_=value.time;
    };
    publish(sample); isLoaded_=true; error_=S_OK;
    Platform::AudioOutput audio; if(reader.HasAudio()) { audio.Open(48000,2,16); audioError_=audio.error(); }
    bool playing=false,muted=isMuted_.load(); uint64_t revision=clockRevision_.load();
    int64_t scrubTarget=-1,scrubReached=-1;
    auto anchor=std::chrono::steady_clock::now(); int64_t media=currentPos100ns_.load(),audioFloor=media;
    auto canSubmit=[&] { return !stopThread_.load() && isPlaying_.load() && !isMuted_.load() && pendingSeek100ns_.load()<0; };
    while(!stopThread_.load()) {
        int64_t target=-1; bool previous=false,exact=true;
        { std::lock_guard lock(stateMutex_); target=pendingSeek100ns_.exchange(-1); previous=seekPreviousFrame_; exact=seekExact_; }
        if(target>=0) {
            audio.Reset(); lastAudioTime100ns_=-1; atEof_=false;
            // An exact seek gives way to a newer one; a scrub stops early and shows the frame it reached.
            auto superseded=[&] { return stopThread_.load() || pendingSeek100ns_.load()>=0; };
            if(reader.VideoAt(target,sample,previous,exact,superseded)) {
                const auto reached=sample.time;
                publish(sample); lastExactTarget100ns_=exact?target:-1;
                // A scrub that ran out of time shows the frame it reached; while the slider rests there, decoding goes
                // on toward it, as long as each pass gets further.
                if(scrubTarget!=target) { scrubTarget=target; scrubReached=-1; }
                if(!exact && !previous && reached<target && reached>scrubReached) {
                    scrubReached=reached; std::lock_guard lock(stateMutex_);
                    if(pendingSeek100ns_.load()<0) { seekPreviousFrame_=false; seekExact_=false; pendingSeek100ns_.store(target); }
                }
            } else if(!superseded()) { error_=E_FAIL; isPlaying_=false; }
            audioFloor=currentPos100ns_.load();
            { std::lock_guard lock(stateMutex_); seeking_=pendingSeek100ns_.load()>=0; }
            playing=false;
            continue;
        }
        if(!isPlaying_.load()) {
            if(playing) audio.Reset(); playing=false;
            std::unique_lock lock(stateMutex_); cv_.wait_for(lock,std::chrono::milliseconds(30),[&] { return stopThread_.load() || isPlaying_.load() || pendingSeek100ns_.load()>=0; });
            continue;
        }
        if(!playing || revision!=clockRevision_.load()) {
            revision=clockRevision_.load(); anchor=std::chrono::steady_clock::now(); media=currentPos100ns_.load();
            if(const auto shared=sharedStartNs_.load();shared>0) { anchor=std::chrono::steady_clock::time_point(std::chrono::nanoseconds(shared)); media=sharedMedia100ns_.load(); }
            audioFloor=media; playing=true;
        }
        if(isMuted_.load() && !muted) audio.Reset();
        if(!isMuted_.load() && muted) audioFloor=currentPos100ns_.load();
        muted=isMuted_.load();
        if(!reader.Next(sample)) {
            if(!stopThread_.load()) { if(reader.Failed()) error_=E_FAIL; else { atEof_=true; currentPos100ns_=duration100ns_.load(); } }
            isPlaying_=false; audio.Reset(); playing=false; continue;
        }
        const auto delta=sample.time-media;
        if(delta>INT64_MAX/100) { error_=E_FAIL; isPlaying_=false; continue; }
        const auto due=anchor+std::chrono::nanoseconds(std::max<int64_t>(0,delta)*100)-(sample.video?std::chrono::milliseconds(0):std::chrono::milliseconds(40));
        while(!stopThread_.load() && isPlaying_.load() && pendingSeek100ns_.load()<0 && std::chrono::steady_clock::now()<due) Platform::SleepShortMs(1);
        if(stopThread_.load() || pendingSeek100ns_.load()>=0) continue;
        if(!isPlaying_.load()) continue;
        if(sample.video) { publish(sample); lastExactTarget100ns_=-1; }
        else if(canSubmit() && sample.time>=audioFloor && !sample.bytes.empty()) {
            if(audio.WritePCM(sample.bytes.data(),uint32_t(sample.bytes.size()),canSubmit)) lastAudioTime100ns_=sample.time;
            audioError_=audio.error();
        }
    }
}
