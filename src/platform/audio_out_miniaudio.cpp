// Audio output on Linux and macOS through miniaudio (PulseAudio/PipeWire/ALSA, CoreAudio).
// Same contract as the waveOut ring on Windows: WritePCM queues 16-bit PCM and waits while the queue is full.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"
#include "platform/PlatformAudioOut.h"
#include <deque>

namespace Platform {

struct AudioOutput::Impl {
    ma_device device{};
    bool isOpen = false;
    std::mutex mutex;
    std::deque<uint8_t> queue;
    size_t capacity = 0;

    static void Callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        auto& self = *static_cast<Impl*>(device->pUserData);
        const size_t wanted = static_cast<size_t>(frames) * ma_get_bytes_per_frame(device->playback.format, device->playback.channels);
        auto* out = static_cast<uint8_t*>(output);
        std::lock_guard lock(self.mutex);
        const size_t available = std::min(wanted, self.queue.size());
        std::copy_n(self.queue.begin(), available, out);
        self.queue.erase(self.queue.begin(), self.queue.begin() + static_cast<std::ptrdiff_t>(available));
        std::fill(out + available, out + wanted, uint8_t(0)); // underrun plays silence
    }
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    Close();
}

bool AudioOutput::Open(uint32_t rate, uint32_t channels, uint32_t bitsPerSample) {
    Close();
    if (rate == 0 || rate > 384000 || channels == 0 || channels > 8 || bitsPerSample != 16) return false;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = channels;
    config.sampleRate = rate;
    config.dataCallback = &Impl::Callback;
    config.pUserData = impl_.get();
    if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) { error_ = 1; return false; }
    if (ma_device_start(&impl_->device) != MA_SUCCESS) { ma_device_uninit(&impl_->device); error_ = 1; return false; }
    sampleRate_ = rate;
    blockAlign_ = channels * 2;
    // About half a second queued, comparable to the 24-buffer waveOut ring.
    impl_->capacity = static_cast<size_t>(rate) * blockAlign_ / 2;
    impl_->isOpen = true;
    error_ = kAudioErrorNone;
    return true;
}

bool AudioOutput::WritePCM(const uint8_t* data, uint32_t len, const std::function<bool()>& canSubmit) {
    if (!impl_->isOpen || !data || len == 0) return false;
    for (;;) {
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->queue.size() + len <= impl_->capacity || impl_->queue.empty()) {
                impl_->queue.insert(impl_->queue.end(), data, data + len);
                return true;
            }
        }
        if (!canSubmit()) return false;
        SleepShortMs(1);
    }
}

void AudioOutput::Reset() {
    if (!impl_->isOpen) return;
    std::lock_guard lock(impl_->mutex);
    impl_->queue.clear();
}

void AudioOutput::Close() {
    if (!impl_ || !impl_->isOpen) return;
    ma_device_uninit(&impl_->device);
    impl_->isOpen = false;
    std::lock_guard lock(impl_->mutex);
    impl_->queue.clear();
}

void SleepShortMs(uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

} // namespace Platform
