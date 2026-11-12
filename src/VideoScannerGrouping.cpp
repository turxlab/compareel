#include "VideoScannerInternal.h"
#include "platform/Platform.h"
#include "FileOperations.h"
#include "ScanConcurrency.h"
#include "ImageCandidates.h"
#include <map>
#include <set>
#include <condition_variable>
#include <future>
#include <thread>

using namespace VideoScannerInternal;

bool VideoScanner::FilesMatch(
    const VideoFileInfo& a,
    const VideoFileInfo& b,
    const ScanCriteria& criteria,
    double* outFrameSim)
{
    if (a.deleted || b.deleted || a.corrupted || b.corrupted) return false;
    if(criteria.crossFolderOnly && (a.comparisonRoot<0 || b.comparisonRoot<0 || a.comparisonRoot==b.comparisonRoot)) return false;
    if(criteria.ignoredPairs.contains(ReviewPairKey(a,b)) || criteria.ignoredPairs.contains(ReviewPairKey(a.identity,b.identity))) return false;
    if (SamePhysicalFile(a, b)) return false;
    if (criteria.matchFileName) {
        if (FileStemLowerW(a.fileName) != FileStemLowerW(b.fileName)) return false;
    }
    if (criteria.matchExtension) {
        if (FileExtLowerW(a.fileName) != FileExtLowerW(b.fileName)) return false;
    }
    if (criteria.matchContent) return !a.contentHash.empty() && a.fileSize == b.fileSize && a.contentHash == b.contentHash;

    if (criteria.matchSize && a.fileSize != b.fileSize) return false;

    if (criteria.matchDuration) {
        if (!a.metadataLoaded || !b.metadataLoaded || a.duration100ns <= 0 || b.duration100ns <= 0) return false;
        int64_t diff = std::abs(a.duration100ns - b.duration100ns);
        if (diff > criteria.durationTol100ns) return false;
    }

    if (criteria.matchResolution) {
        if (!a.metadataLoaded || !b.metadataLoaded) return false;
        if (a.width == 0 || a.height == 0 || b.width == 0 || b.height == 0) return false;
        if (a.width != b.width || a.height != b.height) return false;
    }

    if(criteria.images && criteria.matchFirstFrame) {
        if (a.previewFailed || b.previewFailed || !a.frameLoaded || !b.frameLoaded) return false;
        bool accepted = false;
        double best = PerceptualHash::CompareImageFiles(a, b, criteria.matchTransforms, criteria.minFrameSimilarity, &accepted,!outFrameSim);
        if(outFrameSim) *outFrameSim=best;
        return accepted;
    }
    double sim = 100.0;
    if (a.frameLoaded && b.frameLoaded) {
        sim = PerceptualHash::ComputeVideoFrameSimilarity(a, b);
    }
    if (outFrameSim) *outFrameSim = sim;

    if (criteria.matchVisualSamples) {
        if (a.previewFailed || b.previewFailed || a.samples.size() != 5 || b.samples.size() != 5) return false;
        double best=0; bool accepted=false;
        // A single orientation must explain the entire clip, not a different transform per frame.
        for(int orientation=-1;orientation<(criteria.matchTransforms?8:0);++orientation) {
            unsigned valid=0,matches=0,nearMatches=0; double total=0;
            for(size_t i=0;i<5;++i) {
                const auto& x=a.samples[i]; const auto& y=b.samples[i];
                if(!x.valid || !y.valid) continue;
                if(orientation>=0 && (x.normalized.size()!=8 || y.normalized.size()!=8)) continue;
                const auto& left=orientation<0?x:x.normalized[0]; const auto& right=orientation<0?y:y.normalized[orientation];
                if(!left.valid || !right.valid || left.lowVariance || right.lowVariance) continue;
                double similarity=criteria.matchTransforms?PerceptualHash::CompareSignaturesWithCrop(left,right):PerceptualHash::CompareSignatures(left,right); total+=similarity; ++valid;
                if(similarity>=criteria.minFrameSimilarity) ++matches;
                if(similarity>=criteria.minFrameSimilarity-3.0) ++nearMatches;
            }
            if(valid>=3) {
                best=std::max(best,total/valid);
                accepted|=(matches*5>=valid*4) || (criteria.matchTransforms && (total/valid)>=criteria.minFrameSimilarity && matches*5>=valid*3 && nearMatches==valid);
                if(accepted && !outFrameSim) return true;
            }
        }
        if(outFrameSim) *outFrameSim=best;
        return accepted;
    }

    if (criteria.matchFirstFrame) {
        if (a.previewFailed || b.previewFailed || !a.frameLoaded || !b.frameLoaded || !a.sigFirst.valid || !b.sigFirst.valid) return false;
        if (sim < criteria.minFrameSimilarity) return false;
    }

    return true;
}

void VideoScanner::AnalyzeAndGroup(
    std::vector<VideoFileInfo>& files, const ScanCriteria& criteria,
    std::vector<VideoGroup>& outGroups, std::atomic<bool>& cancelFlag,
    ProgressCallback onProgress,FileProcessedCallback onFileProcessed,ScanMetrics* metrics,std::function<void(VideoFileInfo&)> readImage)
{
    if(metrics) {
        const size_t prevDiscovered=metrics->discoveredFiles;
        const size_t prevOutside=metrics->outsideSizeLimits;
        const size_t prevUnique=metrics->skippedUniqueSize;
        const bool prevCompacted=metrics->compactedFiles;
        *metrics={};
        metrics->discoveredFiles=prevDiscovered;
        metrics->outsideSizeLimits=prevOutside;
        metrics->skippedUniqueSize=prevUnique;
        metrics->compactedFiles=prevCompacted;
    }
    auto phaseStart=std::chrono::steady_clock::now();
    for(auto& file:files) {
        file.comparisonRoot=-1; size_t longest=0;
        for(size_t root=0;root<criteria.comparisonRoots.size();++root) if(criteria.comparisonRoots[root].size()>longest && WithinFolder(file.fullPath,criteria.comparisonRoots[root])) { longest=criteria.comparisonRoots[root].size(); file.comparisonRoot=int(root); }
    }
    auto elapsedMs=[&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-phaseStart).count(); };
    outGroups.clear();
    std::vector<int> candidates;
    std::map<uint64_t, unsigned> sizes;
    size_t outsideLimits = 0;
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        if (files[i].deleted) continue;
        if (files[i].fileSize < criteria.minFileSize || files[i].fileSize > criteria.maxFileSize) { ++outsideLimits; continue; }
        candidates.push_back(i);
        ++sizes[files[i].fileSize];
    }
    size_t uniqueSkipped = 0;
    if (criteria.matchContent || criteria.matchSize) {
        const size_t before = candidates.size();
        std::erase_if(candidates, [&](int index) { return sizes[files[index].fileSize] < 2; });
        uniqueSkipped = before - candidates.size();
    }
    if(metrics) {
        metrics->outsideSizeLimits += outsideLimits;
        metrics->skippedUniqueSize += uniqueSkipped;
        metrics->inputCandidates = candidates.size();
    }
    if (candidates.size() < 2 || cancelFlag.load()) return;

    std::map<uint64_t, StorageTier> volumeTiers;
    for (int index : candidates) {
        const auto& file = files[index];
        uint64_t vol = file.identity.volume;
        if (volumeTiers.contains(vol)) continue;
        auto overrideIt = criteria.volumeTierOverrides.find(vol);
        if (overrideIt != criteria.volumeTierOverrides.end()) {
            volumeTiers[vol] = static_cast<StorageTier>(std::clamp(overrideIt->second, 0, 3));
        } else {
            volumeTiers[vol] = DetectStorageTier(file.fullPath);
        }
    }

    auto parallel = [&](const std::function<void(VideoFileInfo&)>& process, int stage, const wchar_t* label) {
        std::atomic<size_t> done{0};
        // std::thread rather than std::jthread: every worker is joined below anyway, and the libc++ of the Xcode
        // versions on GitHub's macOS machines has no jthread.
        std::vector<std::thread> workers;
        uint64_t available=Platform::AvailablePhysicalMemoryBytes(0), largestPixels=0;
        if(stage==3) for(int index:candidates) largestPixels=std::max(largestPixels,uint64_t(files[index].width)*files[index].height);
        const bool imageDecode=criteria.images && !criteria.matchContent;
        if(imageDecode) largestPixels=std::max<uint64_t>(largestPixels,67108864);
        const bool isDecode=stage==3 || imageDecode;

        std::map<uint64_t, unsigned> volCapMap;
        uint64_t totalVolCap=0;
        StorageTier primaryTier=StorageTier::SolidState;
        bool firstVol=true;
        for(int index:candidates) {
            uint64_t vol=files[index].identity.volume;
            if(!volCapMap.contains(vol)) {
                StorageTier tier=volumeTiers.contains(vol)?volumeTiers[vol]:StorageTier::Unknown;
                if(firstVol) { primaryTier=tier; firstVol=false; }
                unsigned cap=VolumeConcurrencyCap(tier,isDecode);
                volCapMap[vol]=cap;
                totalVolCap+=cap;
            }
        }
        auto budget=LimitScanConcurrency(criteria.workerCount,candidates.size(),std::thread::hardware_concurrency(),available,largestPixels,isDecode,
                                         volCapMap.size()==1?primaryTier:StorageTier::SolidState);
        unsigned count=static_cast<unsigned>(std::min<uint64_t>(budget.workers,std::max<uint64_t>(1,totalVolCap)));
        if(metrics) {
            metrics->requestedWorkers=std::clamp(criteria.workerCount,1u,16u);
            metrics->volumeCap=static_cast<unsigned>(std::min<uint64_t>(16,std::max<uint64_t>(1,totalVolCap)));
            metrics->primaryStorageTier=static_cast<int>(primaryTier);
            auto& selected=stage==3?metrics->sampleWorkers:metrics->readWorkers;
            selected=std::max(selected,count);
        }

        const bool multiVolume = volCapMap.size() > 1;
        std::atomic<size_t> next{0};
        std::mutex schedMutex;
        std::condition_variable schedCv;
        std::vector<uint8_t> claimed(multiVolume ? candidates.size() : 0, 0);
        std::map<uint64_t, unsigned> activePerVol;
        size_t firstUnclaimed = 0;

        auto acquireCandidate = [&](size_t& outPos, uint64_t& outVol) -> bool {
            if (!multiVolume) {
                size_t p = next.fetch_add(1);
                if (p >= candidates.size()) return false;
                outPos = p;
                outVol = files[candidates[p]].identity.volume;
                return true;
            }
            std::unique_lock<std::mutex> lock(schedMutex);
            while (!cancelFlag.load()) {
                while (firstUnclaimed < candidates.size() && claimed[firstUnclaimed]) ++firstUnclaimed;
                if (firstUnclaimed >= candidates.size()) return false;
                bool found = false;
                for (size_t i = firstUnclaimed; i < candidates.size(); ++i) {
                    if (claimed[i]) continue;
                    uint64_t vol = files[candidates[i]].identity.volume;
                    unsigned cap = volCapMap[vol];
                    if (activePerVol[vol] < cap) {
                        claimed[i] = 1;
                        ++activePerVol[vol];
                        outPos = i;
                        outVol = vol;
                        found = true;
                        break;
                    }
                }
                if (found) return true;
                schedCv.wait_for(lock, std::chrono::milliseconds(10));
            }
            return false;
        };

        auto releaseVolume = [&](uint64_t vol) {
            if (!multiVolume) return;
            {
                std::lock_guard<std::mutex> lock(schedMutex);
                auto it = activePerVol.find(vol);
                if (it != activePerVol.end() && it->second > 0) --it->second;
            }
            schedCv.notify_all();
        };

        for (unsigned worker = 0; worker < count; ++worker) workers.emplace_back([&] {
            Platform::ComInit com(Platform::ComApartment::MultiThreaded);
            while (!cancelFlag.load()) {
                size_t position = 0;
                uint64_t vol = 0;
                if (!acquireCandidate(position, vol)) break;
                auto& file = files[candidates[position]];
                try {
                    auto identity=file.identity;
                    if(identity.valid && FileOperations::ReadIdentity(file.fullPath)!=identity) { file.contentHash.clear(); file.corrupted=true; file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID); }
                    else {
                        process(file);
                        if(identity.valid && FileOperations::ReadIdentity(file.fullPath)!=identity) {
                            file.contentHash.clear(); file.corrupted=true; file.error=HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
                        }
                    }
                }
                catch (...) {
                    file.error = E_FAIL;
                    if (file.identity.valid && FileOperations::ReadIdentity(file.fullPath) != file.identity) {
                        file.contentHash.clear();
                        file.corrupted = true;
                    } else {
                        file.previewFailed = true;
                    }
                }
                releaseVolume(vol);
                if(onFileProcessed) onFileProcessed(file);
                size_t completed = done.fetch_add(1) + 1;
                if (onProgress && (completed % 4 == 0 || completed == candidates.size())) {
                    ScanProgress progress;
                    progress.stage = stage;
                    progress.current = static_cast<int>(completed);
                    progress.total = static_cast<int>(candidates.size());
                    progress.statusText = label;
                    onProgress(progress);
                }
            }
        });
        for (auto& worker : workers) worker.join();
    };
    if (criteria.matchContent) {
        parallel([&](VideoFileInfo& file) {
            auto identity = FileOperations::ReadIdentity(file.fullPath);
            if (!identity.valid || (file.identity.valid && identity != file.identity)) {
                file.contentHash.clear(); file.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID); return;
            }
            file.identity = identity;
            if (file.contentHash.empty()) file.contentHash = FileOperations::ContentHash(file.fullPath, cancelFlag, file.error, identity);
            if (FileOperations::ReadIdentity(file.fullPath) != identity) {
                file.contentHash.clear(); file.error = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
            }
        }, 2, L"Verifying file contents");
        std::erase_if(candidates, [&](int index) { return files[index].contentHash.empty(); });
        if(metrics) metrics->readMs=elapsedMs();
    } else {
        bool metadata = criteria.matchDuration || criteria.matchResolution || criteria.matchVisualSamples ||
            criteria.sortOrder == SortOrder::DurationAsc || criteria.sortOrder == SortOrder::DurationDesc ||
            criteria.sortOrder == SortOrder::ResolutionAsc || criteria.sortOrder == SortOrder::ResolutionDesc;
        auto image=[&](VideoFileInfo& file) {
            if((file.frameLoaded && file.metadataLoaded) || file.corrupted || file.previewFailed) return;
            if(readImage) readImage(file);
            else { file.error=E_NOTIMPL; file.previewFailed=true; }
        };
        if (metadata) parallel([&](VideoFileInfo& file) { if(criteria.images) image(file); else ExtractMetadataOnly(file, &cancelFlag); }, 2, L"Reading media information");
        if(criteria.matchDuration && !cancelFlag.load()) {
            auto before=candidates.size();
            std::erase_if(candidates,[&](int index) {
                const auto& file=files[index];
                return file.corrupted || file.previewFailed || !file.metadataLoaded || file.duration100ns<=0;
            });
            std::sort(candidates.begin(),candidates.end(),[&](int a,int b) { return files[a].duration100ns<files[b].duration100ns; });
            std::vector<int> neighbors;
            for(size_t i=0;i<candidates.size();++i) {
                int64_t duration=files[candidates[i]].duration100ns;
                // Sorted adjacent durations suffice to find any possible duration match.
                bool previous=i>0 && duration-files[candidates[i-1]].duration100ns<=criteria.durationTol100ns;
                bool next=i+1<candidates.size() && files[candidates[i+1]].duration100ns-duration<=criteria.durationTol100ns;
                if(previous || next) neighbors.push_back(candidates[i]);
            }
            candidates=std::move(neighbors);
            if(metrics) metrics->durationRejected=before-candidates.size();
        }
        if(metrics) { metrics->readMs=elapsedMs(); metrics->sampleCandidates=candidates.size(); }
        phaseStart=std::chrono::steady_clock::now();
        if (criteria.matchFirstFrame || criteria.matchVisualSamples)
            parallel([&](VideoFileInfo& file) { if(criteria.images) image(file); else ExtractMetadataAndFrame(file, criteria.matchVisualSamples, &cancelFlag); },
                     3, L"Sampling video content");
        if (metadata || criteria.matchFirstFrame || criteria.matchVisualSamples) {
            std::erase_if(candidates, [&](int index) {
                const auto& f = files[index];
                if (f.corrupted) return true;
                if ((criteria.matchFirstFrame || criteria.matchVisualSamples) && (!f.frameLoaded || f.previewFailed)) return true;
                if (criteria.matchDuration && (!f.metadataLoaded || f.duration100ns <= 0)) return true;
                if (criteria.matchResolution && (!f.metadataLoaded || f.width == 0 || f.height == 0)) return true;
                return false;
            });
        }
        if(metrics) metrics->sampleMs=elapsedMs();
    }
    if (candidates.size() < 2 || cancelFlag.load()) return;
    phaseStart=std::chrono::steady_clock::now();

    GroupKnownFiles(files,candidates,criteria,outGroups,cancelFlag,onProgress);
    if (cancelFlag.load()) { outGroups.clear(); return; }
    if(metrics) metrics->groupMs=elapsedMs();
    if (criteria.loadPreviews) {
        std::set<int> members;
        for (const auto& group : outGroups) members.insert(group.fileIndices.begin(), group.fileIndices.end());
        candidates.assign(members.begin(), members.end());
        if (!candidates.empty()) parallel([&](VideoFileInfo& file) {
            if (criteria.images) {
                if (readImage && (!file.frameLoaded || !file.metadataLoaded) && !file.corrupted && !file.previewFailed) readImage(file);
            } else {
                if (!file.corrupted && !file.previewFailed) ExtractMetadataAndFrame(file, false, &cancelFlag);
            }
        }, 3, L"Preparing previews");
    }
    SortGroups(files, outGroups, criteria.sortOrder,criteria.matchTransforms);
}

void VideoScanner::GroupKnownFiles(const std::vector<VideoFileInfo>& files,std::vector<int> candidates,
    const ScanCriteria& criteria,std::vector<VideoGroup>& outGroups,std::atomic<bool>& cancelFlag,ProgressCallback onProgress) {
    outGroups.clear();
    std::erase_if(candidates,[&](int idx) {
        if(idx<0 || static_cast<size_t>(idx)>=files.size()) return true;
        const auto& f=files[idx];
        if(f.deleted) return true;
        if(criteria.matchContent) return f.contentHash.empty();
        if(f.corrupted) return true;
        if(criteria.matchDuration && (!f.metadataLoaded || f.duration100ns<=0)) return true;
        if(criteria.matchResolution && (!f.metadataLoaded || f.width==0 || f.height==0)) return true;
        if(criteria.matchVisualSamples && (f.previewFailed || f.samples.size()!=5)) return true;
        if(criteria.matchFirstFrame && !criteria.matchVisualSamples && (f.previewFailed || !f.frameLoaded || !f.sigFirst.valid)) return true;
        return false;
    });
    if(candidates.size()<2) return;
    const SortOrder preSortOrder =
        (criteria.sortOrder == SortOrder::SimilarityDesc || criteria.sortOrder == SortOrder::SimilarityAsc)
        ? SortOrder::SizeDesc
        : criteria.sortOrder;
    VideoGroup ranked;
    ranked.fileIndices = candidates;
    std::vector<VideoGroup> ranking{ranked};
    SortGroups(files, ranking, preSortOrder, criteria.matchTransforms);
    candidates = std::move(ranking[0].fileIndices);
    std::unique_ptr<ImageCandidates> imageCandidates;
    const auto temporal=[&](size_t pos) { const auto kind=files[candidates[pos]].imageKind; return kind==ImageEvidenceKind::AnimatedImage || kind==ImageEvidenceKind::MultiPageDocument; };
    std::vector<size_t> temporalPositions;
    for(size_t i=0;i<candidates.size();++i) if(temporal(i)) temporalPositions.push_back(i);
    if(criteria.images && criteria.matchFirstFrame && !criteria.matchContent && candidates.size()-temporalPositions.size()>=64) {
        const size_t approxPoints=candidates.size()*(criteria.matchTransforms?18:1);
        const uint64_t bucketBytes=ImageCandidates::usesBucketIndex(criteria.minFrameSimilarity,approxPoints)
            ?(4ULL*12*65536+approxPoints*48ULL):0ULL;
        const uint64_t estimate=bucketBytes+approxPoints*32ULL;
        if(Platform::AvailablePhysicalMemoryBytes(0)>estimate*3) {
            try { imageCandidates=std::make_unique<ImageCandidates>(files,candidates,criteria.matchTransforms,cancelFlag,criteria.minFrameSimilarity); }
            catch(const std::bad_alloc&) { imageCandidates.reset(); }
        }
    }
    // Related-image components retain direct matches; cleanup checks each pair.
    if(!criteria.crossFolderOnly && criteria.images && criteria.matchFirstFrame && !criteria.matchContent) {
        std::vector<size_t> parent(candidates.size());
        for(size_t i=0;i<parent.size();++i) parent[i]=i;
        auto root=[&](size_t i) { while(parent[i]!=i) { parent[i]=parent[parent[i]]; i=parent[i]; } return i; };
        std::atomic<size_t> next{0}; std::atomic<bool> abort{false};
        std::mutex groupingMutex; size_t completed=0;
        auto work=[&] {
          try {
            ImageCandidates::Scratch scratch;
            std::vector<size_t> window;
            std::vector<size_t> localMatches;
            for(;;) {
              const size_t i=next.fetch_add(1,std::memory_order_relaxed);
              if(i>=candidates.size() || cancelFlag.load(std::memory_order_relaxed) || abort.load(std::memory_order_relaxed)) return;
              localMatches.clear();
              // Animations/pages keep full temporal evidence without disabling the
              // static-image index for the rest of a mixed archive.
              if(temporal(i)) {
                  for(auto j:temporalPositions) {
                      if(cancelFlag.load(std::memory_order_relaxed)) return;
                      if(j>i && files[candidates[i]].imageKind==files[candidates[j]].imageKind && FilesMatch(files[candidates[i]],files[candidates[j]],criteria))
                          localMatches.push_back(j);
                  }
              } else if(imageCandidates) {
                  // Query the canonical orientation at BOTH endpoints. The reverse
                  // query covers a rotated query frame; never discard j<i here.
                  imageCandidates->query(files[candidates[i]],criteria.minFrameSimilarity,true,scratch,window);
                  for(auto j:window) {
                      if(cancelFlag.load(std::memory_order_relaxed)) return;
                      if(j!=i && FilesMatch(files[candidates[i]],files[candidates[j]],criteria)) localMatches.push_back(j);
                  }
              } else {
                  for(size_t j=i+1;j<candidates.size();++j) {
                      if(cancelFlag.load(std::memory_order_relaxed)) return;
                      if(!temporal(j) && FilesMatch(files[candidates[i]],files[candidates[j]],criteria)) localMatches.push_back(j);
                  }
              }
              size_t doneNow=0;
              {
                  std::lock_guard lock(groupingMutex);
                  for(size_t j:localMatches) {
                      const auto a=root(i),b=root(j);
                      if(a!=b) { if(a<b) parent[b]=a; else parent[a]=b; }
                  }
                  doneNow=++completed;
              }
              if(onProgress && doneNow%100==0) { ScanProgress p; p.stage=4; p.current=int(doneNow); p.total=int(candidates.size()); p.statusText=L"Grouping related images"; onProgress(p); }
            }
          } catch(...) { abort=true; throw; }
        };
        const unsigned hw=std::max(1u,std::thread::hardware_concurrency());
        const unsigned workers=candidates.size()>=128?std::clamp(criteria.workerCount,1u,std::min(16u,hw)):1u;
        std::vector<std::future<void>> tasks;
        for(unsigned i=1;i<workers;++i) tasks.push_back(std::async(std::launch::async,work));
        work(); for(auto& task:tasks) task.get();
        if(cancelFlag.load()) return;
        std::map<size_t,VideoGroup> components;
        for(size_t i=0;i<candidates.size();++i) components[root(i)].fileIndices.push_back(candidates[i]);
        for(auto& [key,group]:components) if(group.fileIndices.size()>1) outGroups.push_back(std::move(group));
        SortGroups(files,outGroups,criteria.sortOrder,criteria.matchTransforms);
        return;
    }
    bool sizeOnly = criteria.matchSize && !criteria.matchDuration && !criteria.matchResolution &&
        !criteria.matchFirstFrame && !criteria.matchVisualSamples && !criteria.matchFileName && !criteria.matchExtension;
    bool resolutionOnly = criteria.matchResolution && !criteria.matchSize && !criteria.matchDuration &&
        !criteria.matchFirstFrame && !criteria.matchVisualSamples && !criteria.matchFileName && !criteria.matchExtension;
    if (!criteria.crossFolderOnly && ((criteria.matchContent && !criteria.matchFileName && !criteria.matchExtension) || sizeOnly || resolutionOnly)) {
        std::map<std::wstring, std::vector<size_t>> groupsByKey;
        for (int index : candidates) {
            const auto& file = files[index];
            std::wstring key = criteria.matchContent
                ? (std::to_wstring(file.fileSize) + L":" + std::wstring(file.contentHash.begin(), file.contentHash.end()))
                : sizeOnly ? std::to_wstring(file.fileSize) : std::to_wstring(file.width) + L"x" + std::to_wstring(file.height);
            auto& possible=groupsByKey[key];
            auto compatible=std::find_if(possible.begin(),possible.end(),[&](size_t group) {
                const auto& ref = files[outGroups[group].fileIndices[0]];
                return FilesMatch(ref, file, criteria);
            });
            if(compatible==possible.end()) { possible.push_back(outGroups.size()); outGroups.emplace_back(); outGroups.back().fileIndices.push_back(index); }
            else outGroups[*compatible].fileIndices.push_back(index);
        }
        std::erase_if(outGroups, [](const VideoGroup& group) { return group.fileIndices.size() < 2; });
    } else if(criteria.crossFolderOnly) {
        std::vector<bool> covered(candidates.size(),false);
        std::vector<std::pair<int64_t, size_t>> byDuration;
        if (criteria.matchDuration && candidates.size() >= 64) {
            byDuration.reserve(candidates.size());
            for (size_t pos = 0; pos < candidates.size(); ++pos) {
                const auto& f = files[candidates[pos]];
                if (f.metadataLoaded && f.duration100ns > 0) byDuration.emplace_back(f.duration100ns, pos);
            }
            std::sort(byDuration.begin(), byDuration.end());
        }
        ImageCandidates::Scratch scratch;
        std::vector<size_t> window;
        for(size_t i=0;i<candidates.size() && !cancelFlag.load();++i) {
            if(covered[i]) continue;
            size_t anchor=i;
            if(criteria.images && criteria.matchFirstFrame && temporal(i)) {
                for(size_t j:temporalPositions) if(j<i && files[candidates[i]].imageKind==files[candidates[j]].imageKind && FilesMatch(files[candidates[i]],files[candidates[j]],criteria)) { anchor=j; break; }
            } else if(imageCandidates && !temporal(i)) {
                imageCandidates->query(files[candidates[i]],criteria.minFrameSimilarity,false,scratch,window);
                for(size_t j:window) if(j<i && FilesMatch(files[candidates[i]],files[candidates[j]],criteria)) { anchor=j; break; }
            } else if (!byDuration.empty()) {
                const int64_t dur = files[candidates[i]].duration100ns;
                if (dur <= 0 || !files[candidates[i]].metadataLoaded) continue;
                auto lo = std::lower_bound(byDuration.begin(), byDuration.end(), std::make_pair(dur - criteria.durationTol100ns, size_t(0)));
                auto hi = std::upper_bound(byDuration.begin(), byDuration.end(), std::make_pair(dur + criteria.durationTol100ns, SIZE_MAX));
                window.clear();
                for (auto it = lo; it != hi; ++it) if (it->second < i) window.push_back(it->second);
                std::sort(window.begin(), window.end());
                for (size_t j : window) if (FilesMatch(files[candidates[i]], files[candidates[j]], criteria)) { anchor = j; break; }
            } else {
                for(size_t j=0;j<i;++j) if(FilesMatch(files[candidates[i]],files[candidates[j]],criteria)) { anchor=j; break; }
            }
            VideoGroup group; group.fileIndices.push_back(candidates[anchor]);
            if(criteria.images && criteria.matchFirstFrame && temporal(anchor)) {
                for(size_t j:temporalPositions) {
                    if(cancelFlag.load()) return;
                    if(j>anchor && files[candidates[anchor]].imageKind==files[candidates[j]].imageKind && FilesMatch(files[candidates[anchor]],files[candidates[j]],criteria)) { group.fileIndices.push_back(candidates[j]); covered[j]=true; }
                }
            } else if(imageCandidates && !temporal(anchor)) {
                imageCandidates->query(files[candidates[anchor]],criteria.minFrameSimilarity,false,scratch,window);
                for(size_t j:window) {
                    if(cancelFlag.load()) return;
                    if(j>anchor && FilesMatch(files[candidates[anchor]],files[candidates[j]],criteria)) { group.fileIndices.push_back(candidates[j]); covered[j]=true; }
                }
            } else if (!byDuration.empty()) {
                const int64_t adur = files[candidates[anchor]].duration100ns;
                auto lo = std::lower_bound(byDuration.begin(), byDuration.end(), std::make_pair(adur - criteria.durationTol100ns, size_t(0)));
                auto hi = std::upper_bound(byDuration.begin(), byDuration.end(), std::make_pair(adur + criteria.durationTol100ns, SIZE_MAX));
                window.clear();
                for (auto it = lo; it != hi; ++it) if (it->second > anchor) window.push_back(it->second);
                std::sort(window.begin(), window.end());
                for (size_t j : window) {
                    if (cancelFlag.load()) return;
                    if (FilesMatch(files[candidates[anchor]], files[candidates[j]], criteria)) { group.fileIndices.push_back(candidates[j]); covered[j] = true; }
                }
            } else {
                for(size_t j=anchor+1;j<candidates.size();++j) {
                    if(cancelFlag.load()) return;
                    if(FilesMatch(files[candidates[anchor]],files[candidates[j]],criteria)) { group.fileIndices.push_back(candidates[j]); covered[j]=true; }
                }
            }
            if(group.fileIndices.size()>=2) { covered[anchor]=true; outGroups.push_back(std::move(group)); }
        }
    } else {
        std::vector<bool> assigned(candidates.size(), false);
        std::vector<std::pair<int64_t, size_t>> byDuration;
        if (criteria.matchDuration && candidates.size() >= 64) {
            byDuration.reserve(candidates.size());
            for (size_t pos = 0; pos < candidates.size(); ++pos) {
                const auto& f = files[candidates[pos]];
                if (f.metadataLoaded && f.duration100ns > 0) byDuration.emplace_back(f.duration100ns, pos);
            }
            std::sort(byDuration.begin(), byDuration.end());
        }
        ImageCandidates::Scratch scratch;
        std::vector<size_t> window;
        for (size_t i = 0; i < candidates.size() && !cancelFlag.load(); ++i) {
            if (assigned[i]) continue;
            VideoGroup group;
            group.fileIndices.push_back(candidates[i]);
            if(imageCandidates && !temporal(i)) {
                imageCandidates->query(files[candidates[i]],criteria.minFrameSimilarity,false,scratch,window);
                for(size_t j:window) {
                    if(cancelFlag.load()) return;
                    if(j>i && !assigned[j] && FilesMatch(files[candidates[i]],files[candidates[j]],criteria)) { group.fileIndices.push_back(candidates[j]); assigned[j]=true; }
                }
            } else if (!byDuration.empty()) {
                const int64_t dur = files[candidates[i]].duration100ns;
                if (dur > 0 && files[candidates[i]].metadataLoaded) {
                    auto lo = std::lower_bound(byDuration.begin(), byDuration.end(), std::make_pair(dur - criteria.durationTol100ns, size_t(0)));
                    auto hi = std::upper_bound(byDuration.begin(), byDuration.end(), std::make_pair(dur + criteria.durationTol100ns, SIZE_MAX));
                    if (static_cast<size_t>(hi - lo) * 2 < candidates.size() - i) {
                        window.clear();
                        for (auto it = lo; it != hi; ++it) if (it->second > i && !assigned[it->second]) window.push_back(it->second);
                        std::sort(window.begin(), window.end());
                        for (size_t j : window) {
                            if (cancelFlag.load()) break;
                            if (!assigned[j] && FilesMatch(files[candidates[i]], files[candidates[j]], criteria)) {
                                group.fileIndices.push_back(candidates[j]);
                                assigned[j] = true;
                            }
                        }
                    } else {
                        for (size_t j = i+1; j < candidates.size() && !cancelFlag.load(); ++j) {
                            if (!assigned[j] && FilesMatch(files[candidates[i]], files[candidates[j]], criteria)) {
                                group.fileIndices.push_back(candidates[j]);
                                assigned[j] = true;
                            }
                        }
                    }
                }
            } else {
                for (size_t j = i+1; j < candidates.size() && !cancelFlag.load(); ++j) {
                    if (!assigned[j] && FilesMatch(files[candidates[i]], files[candidates[j]], criteria)) {
                        group.fileIndices.push_back(candidates[j]);
                        assigned[j] = true;
                    }
                }
            }
            if (group.fileIndices.size() >= 2) outGroups.push_back(std::move(group));
            if (onProgress && i % 100 == 0) {
                ScanProgress progress;
                progress.stage = 4; progress.current = static_cast<int>(i); progress.total = static_cast<int>(candidates.size());
                progress.statusText = L"Grouping direct matches";
                onProgress(progress);
            }
        }
    }
    if(cancelFlag.load()) outGroups.clear();
    else if(criteria.sortOrder == SortOrder::SimilarityDesc || criteria.sortOrder == SortOrder::SimilarityAsc) {
        SortGroups(files, outGroups, criteria.sortOrder, criteria.matchTransforms);
    }
}
