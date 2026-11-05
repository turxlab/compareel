#pragma once
#include "Common.h"

namespace Platform {

// Audio device error: 0 means none; other values are backend-specific (waveOut MMRESULT, miniaudio result).
using AudioError = uint32_t;
inline constexpr AudioError kAudioErrorNone = 0;

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Open(uint32_t rate, uint32_t channels, uint32_t bitsPerSample);
    bool WritePCM(const uint8_t* data, uint32_t len, const std::function<bool()>& canSubmit);
    void Reset();
    void Close();

    uint32_t sampleRate() const { return sampleRate_; }
    uint32_t blockAlign() const { return blockAlign_; }
    AudioError error() const { return error_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    uint32_t sampleRate_ = 0;
    uint32_t blockAlign_ = 0;
    AudioError error_ = kAudioErrorNone;
};

void SleepShortMs(uint32_t milliseconds);

} // namespace Platform
