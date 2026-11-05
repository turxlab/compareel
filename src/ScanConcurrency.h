#pragma once
#include "platform/Platform.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

inline const wchar_t* StorageTierLabel(StorageTier tier) {
    switch (tier) {
        case StorageTier::Rotational: return L"HDD";
        case StorageTier::NetworkOrRemovable: return L"Network/Removable";
        case StorageTier::SolidState: return L"SSD";
        case StorageTier::Unknown:
        default: return L"Unknown (conservative)";
    }
}

inline const char* StorageTierKey(int tier) {
    switch (static_cast<StorageTier>(tier)) {
        case StorageTier::Rotational: return "HDD (rotational)";
        case StorageTier::NetworkOrRemovable: return "Network / removable";
        case StorageTier::SolidState: return "SSD / NVMe";
        case StorageTier::Unknown:
        default: return "Unknown (conservative)";
    }
}

inline unsigned VolumeConcurrencyCap(StorageTier tier, bool decode) {
    switch (tier) {
        case StorageTier::Rotational:
            // Avoid rotational head-thrashing when seeking/decoding multiple streams on the same spindle.
            return decode ? 1u : 2u;
        case StorageTier::NetworkOrRemovable:
        case StorageTier::Unknown:
            // Conservative per-volume cap when medium is remote, removable, or unknown.
            return 2u;
        case StorageTier::SolidState:
            return decode ? 8u : 16u;
    }
    return 2u;
}

inline StorageTier DetectStorageTier(const std::wstring& path) {
    return Platform::DetectStorageTier(path);
}

struct ScanConcurrency {
    unsigned workers=0;
    uint64_t estimatedBytesPerWorker=0;
    unsigned volumeCap=16;
    StorageTier tier=StorageTier::SolidState;
};

inline ScanConcurrency LimitScanConcurrency(unsigned requested,size_t candidates,unsigned processors,
                                           uint64_t availableBytes,uint64_t largestPixels,bool decode,
                                           StorageTier tier=StorageTier::SolidState,size_t distinctVolumes=1) {
    if(!candidates) return {};
    constexpr uint64_t MiB=1024*1024;
    uint64_t estimate=16*MiB;
    if(decode) {
        // Decoder heaps vary; reserve headroom plus eight BGRA buffers, not a hard RAM guarantee.
        largestPixels=std::max<uint64_t>(largestPixels,3840ULL*2160);
        estimate=largestPixels>(UINT64_MAX-256*MiB)/32?UINT64_MAX:256*MiB+largestPixels*32;
    }
    unsigned cpuLimit=std::max(1u,decode?processors/2:processors);
    uint64_t memoryLimit=std::max<uint64_t>(1,availableBytes/2/estimate);
    unsigned volCap=VolumeConcurrencyCap(tier,decode);
    uint64_t totalVolCap=static_cast<uint64_t>(volCap)*std::max<size_t>(1,distinctVolumes);
    return {static_cast<unsigned>(std::min<uint64_t>({std::clamp(requested,1u,16u),candidates,cpuLimit,memoryLimit,totalVolCap})),estimate,volCap,tier};
}
