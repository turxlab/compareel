#pragma once
#include "Common.h"

namespace Platform {

struct WicDecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    int orientation = 1;
    std::vector<uint8_t> bgra;
};

bool DecodeWicImage(const uint8_t* data,
                    size_t size,
                    std::atomic<bool>& cancel,
                    HRESULT& error,
                    WicDecodedImage& out);

} // namespace Platform
