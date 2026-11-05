#pragma once
#include "Common.h"

namespace AudioEvidence {
enum class Status { SimilarSamples, DifferentSamples, Inconclusive, NeitherHasAudio, OneHasAudio, Failed, Cancelled };
struct Result { Status status=Status::Failed; unsigned windows=0; double lowestCorrelation=0; int leftTracks=0,rightTracks=0; };
// Positive offset means the right video's matching content occurs later.
Result Compare(const VideoFileInfo& left,const VideoFileInfo& right,int64_t offset,std::atomic<bool>& cancel);
}
