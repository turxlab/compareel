#include "PartialClipSearch.h"
#include "FFmpegReader.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include "PerceptualHash.h"
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace PartialClipSearch {
Result Find(const std::vector<std::wstring>& folders,ScanCriteria criteria,std::atomic<bool>& cancel,ProgressCallback progress) {
    Result result; criteria.images=false;
    std::vector<ScanIssue> issues;
    auto discovered=VideoScanner::DiscoverVideos(folders,criteria.includeSubfolders,cancel,progress,&criteria,&issues);
    const auto& roots=criteria.comparisonRoots.empty()?folders:criteria.comparisonRoots;
    for(auto& d:discovered) {
        d.comparisonRoot=-1; size_t longest=0;
        for(size_t r=0;r<roots.size();++r) {
            if(roots[r].size()>longest && VideoScanner::IsWithinFolder(d.fullPath,roots[r])) {
                longest=roots[r].size(); d.comparisonRoot=static_cast<int>(r);
            }
        }
    }
    result.failed=unsigned(issues.size());
    struct Point { uint32_t file,sample; };
    std::unordered_map<unsigned,std::vector<Point>> index;
    std::vector<std::vector<FrameSignature>> timelines;
    size_t total=0; constexpr size_t maxFrames=500000,maxFiles=5000;
    for(size_t input=0;input<discovered.size() && !cancel.load();++input) {
        if(progress) { ScanProgress p; p.current=int(input); p.total=int(discovered.size()); p.statusText=L"Sampling partial-clip timelines"; progress(p); }
        auto file=std::move(discovered[input]);
        if(total>=maxFrames || result.files.size()>=maxFiles) { result.skipped+=unsigned(discovered.size()-input); result.limited=true; break; }
        // Same open as ReadIdentity: long-path safe, and a link is locked as itself rather than followed.
        Platform::ReadLock lock(file.fullPath);
        FFmpegReader reader;
        if(!lock.valid() || !file.identity.valid || FileOperations::ReadIdentity(file.fullPath)!=file.identity || !reader.Open(file.fullPath,&cancel) || !reader.Metadata(file)) { ++result.failed; continue; }
        // Fixed spacing permits an offset index across files with different durations.
        if(file.duration100ns<60000000 || file.duration100ns>7200LL*10000000) { ++result.skipped; continue; }
        const size_t needed=size_t(file.duration100ns/10000000)+1;
        if(needed>maxFrames-total) { ++result.skipped; result.limited=true; continue; }
        std::vector<FrameSignature> samples;
        if(!reader.SampleTimeline(10000000,needed,samples,0) || FileOperations::ReadIdentity(file.fullPath)!=file.identity) { ++result.failed; continue; }
        const uint32_t id=uint32_t(result.files.size());
        // Offset votes are sparse; popular/static buckets are deliberately not expanded quadratically.
        std::map<std::pair<uint32_t,int>,unsigned> votes;
        for(uint32_t at=0;at<samples.size() && !cancel.load();++at) {
            const auto& s=samples[at]; if(!s.valid || s.lowVariance) continue;
            std::unordered_set<uint64_t> seen;
            for(unsigned band=0;band<8;++band) {
                const unsigned key=(band<<12)|unsigned(std::rotr(s.dHashH,int(band*8))&4095);
                const auto found=index.find(key); if(found==index.end()) continue;
                if(found->second.size()>256) { result.limited=true; continue; }
                for(const auto p:found->second) {
                    const uint64_t point=(uint64_t(p.file)<<32)|p.sample; if(!seen.insert(point).second) continue;
                    const auto& other=timelines[p.file][p.sample];
                    if(PerceptualHash::CompareSignaturesWithCrop(s,other)<90) continue;
                    const int shift=int(std::llround(double(s.timestamp100ns-other.timestamp100ns)/10000000));
                    const auto keyShift=std::make_pair(p.file,shift);
                    if(votes.size()<20000 || votes.contains(keyShift)) ++votes[keyShift]; else result.limited=true;
                }
            }
        }
        struct Run { Match match; unsigned count=0; };
        std::map<uint32_t,std::vector<Run>> candidates;
        for(const auto& [key,count]:votes) {
            if(cancel.load()) break;
            if(count<6) continue;
            if(SamePhysicalFile(result.files[key.first],file)) continue;
            if(criteria.ignoredPairs.contains(ReviewPairKey(result.files[key.first],file)) ||
               criteria.ignoredPairs.contains(ReviewPairKey(result.files[key.first].identity,file.identity))) continue;
            const auto& before=timelines[key.first];
            if(criteria.crossFolderOnly && (result.files[key.first].comparisonRoot<0 || file.comparisonRoot<0 || result.files[key.first].comparisonRoot==file.comparisonRoot)) continue;
            Run best,current; double sum=0;
            auto commit=[&] { if(current.count>=6 && current.count>best.count) { current.match.similarity=sum/current.count; best=current; } current={}; sum=0; };
            size_t right=0;
            for(size_t left=0;left<before.size();++left) {
                const auto target=before[left].timestamp100ns+int64_t(key.second)*10000000;
                while(right<samples.size() && samples[right].timestamp100ns<target-4000000) ++right;
                if(right>=samples.size() || std::abs(samples[right].timestamp100ns-target)>4000000 || !before[left].valid || !samples[right].valid || before[left].lowVariance || samples[right].lowVariance) { commit(); continue; }
                const double similarity=PerceptualHash::CompareSignaturesWithCrop(before[left],samples[right]);
                if(similarity<90) { commit(); continue; }
                if(!current.count) { current.match={int(key.first),int(id),int64_t(key.second)*10000000,before[left].timestamp100ns,0,0}; }
                current.match.duration=before[left].timestamp100ns-current.match.start+10000000; ++current.count; sum+=similarity;
            }
            commit(); if(best.count<6) continue;
            const auto first=size_t(std::llround(double(best.match.start)/10000000));
            const auto last=std::min(before.size()-1,first+best.count-1);
            if(first>=before.size() || PerceptualHash::CompareSignatures(before[first],before[last])>99.5) { ++result.ambiguous; continue; }
            candidates[key.first].push_back(best);
        }
        for(auto& [prior,runs]:candidates) {
            std::sort(runs.begin(),runs.end(),[](const Run& a,const Run& b) { return a.count>b.count; });
            const auto& best=runs.front(); bool ambiguous=false;
            for(size_t i=1;i<runs.size();++i) if(std::abs(runs[i].match.offset-best.match.offset)>=20000000 && runs[i].count>=best.count*0.9) ambiguous=true;
            if(ambiguous) { ++result.ambiguous; continue; }
            if(result.matches.size()<500) result.matches.push_back(best.match); else result.limited=true;
        }
        total+=samples.size();
        for(uint32_t at=0;at<samples.size();++at) if(samples[at].valid && !samples[at].lowVariance) for(unsigned band=0;band<8;++band) {
            auto& bucket=index[(band<<12)|unsigned(std::rotr(samples[at].dHashH,int(band*8))&4095)];
            if(bucket.size()<=256) bucket.push_back({id,at});
        }
        file.thumbBGRA.clear(); file.samples.clear(); result.files.push_back(std::move(file)); timelines.push_back(std::move(samples));
    }
    result.cancelled=cancel.load(); if(result.cancelled) result.matches.clear();
    return result;
}
}
