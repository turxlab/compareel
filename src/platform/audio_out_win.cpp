#include "platform/WinApi.h"
#include "platform/PlatformAudioOut.h"

namespace Platform {

struct AudioOutput::Impl {
    static constexpr int NUM_BUFS = 24;
    HWAVEOUT hWaveOut = nullptr;
    WAVEHDR headers[NUM_BUFS]{};
    std::vector<uint8_t> buffers[NUM_BUFS];
    int nextBuf = 0;
    bool isOpen = false;
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}

AudioOutput::~AudioOutput() {
    Close();
}

bool AudioOutput::Open(uint32_t rate, uint32_t channels, uint32_t bitsPerSample) {
    Close();
    if (rate == 0 || rate > 384000 || channels == 0 || channels > 8 || bitsPerSample != 16) return false;

    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(channels);
    wfx.nSamplesPerSec = rate;
    wfx.wBitsPerSample = static_cast<WORD>(bitsPerSample);
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    error_ = waveOutOpen(&impl_->hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (error_ != MMSYSERR_NOERROR) {
        impl_->hWaveOut = nullptr;
        return false;
    }
    impl_->isOpen = true;
    sampleRate_ = rate;
    blockAlign_ = wfx.nBlockAlign;
    impl_->nextBuf = 0;
    return true;
}

bool AudioOutput::WritePCM(const uint8_t* data, uint32_t len, const std::function<bool()>& canSubmit) {
    if (!impl_->isOpen || !impl_->hWaveOut || !data || len == 0) return false;

    WAVEHDR& hdr = impl_->headers[impl_->nextBuf];
    while ((hdr.dwFlags & WHDR_INQUEUE) && canSubmit()) Sleep(1);
    if (!canSubmit()) return false;
    if (hdr.dwFlags & WHDR_PREPARED) {
        waveOutUnprepareHeader(impl_->hWaveOut, &hdr, sizeof(WAVEHDR));
    }

    impl_->buffers[impl_->nextBuf].assign(data, data + len);
    ZeroMemory(&hdr, sizeof(WAVEHDR));
    hdr.lpData = reinterpret_cast<LPSTR>(impl_->buffers[impl_->nextBuf].data());
    hdr.dwBufferLength = len;

    error_ = waveOutPrepareHeader(impl_->hWaveOut, &hdr, sizeof(WAVEHDR));
    if (error_ == MMSYSERR_NOERROR) error_ = waveOutWrite(impl_->hWaveOut, &hdr, sizeof(WAVEHDR));
    impl_->nextBuf = (impl_->nextBuf + 1) % Impl::NUM_BUFS;
    return error_ == MMSYSERR_NOERROR;
}

void AudioOutput::Reset() {
    if (impl_->isOpen && impl_->hWaveOut) {
        waveOutReset(impl_->hWaveOut);
    }
}

void AudioOutput::Close() {
    if (!impl_) return;
    if (impl_->isOpen && impl_->hWaveOut) {
        waveOutReset(impl_->hWaveOut);
        for (int i = 0; i < Impl::NUM_BUFS; ++i) {
            if (impl_->headers[i].dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(impl_->hWaveOut, &impl_->headers[i], sizeof(WAVEHDR));
            }
            ZeroMemory(&impl_->headers[i], sizeof(WAVEHDR));
            impl_->buffers[i].clear();
        }
        waveOutClose(impl_->hWaveOut);
        impl_->hWaveOut = nullptr;
    }
    impl_->isOpen = false;
}

void SleepShortMs(uint32_t milliseconds) {
    Sleep(milliseconds);
}

} // namespace Platform
