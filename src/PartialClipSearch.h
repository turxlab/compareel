#pragma once
#include "VideoScanner.h"

namespace PartialClipSearch {
struct Match { int left=0,right=0; int64_t offset=0,start=0,duration=0; double similarity=0; };
struct Result {
    std::vector<VideoFileInfo> files; std::vector<Match> matches;
    unsigned skipped=0,failed=0,ambiguous=0; bool limited=false,cancelled=false;
};
// Separate review candidates: never fed into duplicate groups or cleanup plans.
Result Find(const std::vector<std::wstring>& folders,ScanCriteria criteria,std::atomic<bool>& cancel,ProgressCallback progress={});
}
