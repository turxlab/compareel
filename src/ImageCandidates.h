#pragma once
#include "Common.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <vector>

// Exact multi-index search. A high-scoring frame must also pass the structural
// Hamming bound; pigeonhole search across twelve 16-bit blocks cannot lose it.
class ImageCandidates {
    struct Point { std::array<uint64_t,3> bits; size_t position; };
    static constexpr int Blocks=12, Buckets=65536;
    std::vector<Point> points_;
    std::vector<int> heads_,next_;
    std::vector<int> searchBlocks_;
    size_t positions_=0;
    std::atomic<bool>& cancel_;
    bool transforms_;
    static Point point(const FrameSignature& s,size_t position,bool crop) {
        return {{crop?s.centerDHashH:s.dHashH,crop?s.centerDHashV:s.dHashV,crop?s.centerAHash:s.aHash},position};
    }
    static uint64_t hash(const Point& p,int axis) { return p.bits[axis]; }
    static unsigned key(const Point& p,int block) { return unsigned((hash(p,block/4)>>(16*(block%4)))&65535); }
    static unsigned difference(const Point& a,const Point& b) {
        return std::popcount(hash(a,0)^hash(b,0))+std::popcount(hash(a,1)^hash(b,1))+std::popcount(hash(a,2)^hash(b,2));
    }
public:
    struct Scratch {
        std::vector<uint32_t> visited;
        std::vector<uint8_t> returned;
        std::vector<std::array<uint64_t,3>> queried;
        uint32_t stamp=0;
    };
    static unsigned maxHammingDifference(double threshold) {
        return static_cast<unsigned>(std::max(0.0,std::floor(192.0*(100.0-threshold)/65.0+1e-7)));
    }
    static bool usesBucketIndex(double threshold,size_t pointCount) {
        if(pointCount==0) return false;
        const unsigned maxDiff=maxHammingDifference(threshold);
        const unsigned minRadius=maxDiff/unsigned(Blocks);
        if(minRadius>3) return false;
        if(minRadius==3 && pointCount<=16384) return false;
        return true;
    }
    ImageCandidates(const std::vector<VideoFileInfo>& files,const std::vector<int>& candidates,
                    bool transforms,std::atomic<bool>& cancel,double threshold=90.0):cancel_(cancel),transforms_(transforms) {
        positions_=candidates.size();
        for(size_t pos=0;pos<candidates.size() && !cancel_.load();++pos) {
            const auto& file=files[candidates[pos]];
            if(!file.frameLoaded || file.corrupted || file.previewFailed || file.imageKind==ImageEvidenceKind::AnimatedImage || file.imageKind==ImageEvidenceKind::MultiPageDocument) continue;
            auto add=[&](const FrameSignature& signature) {
                if(!signature.valid || signature.lowVariance) return;
                points_.push_back(point(signature,pos,false));
                if(transforms && signature.hasCenterCrop) points_.push_back(point(signature,pos,true));
            };
            add(file.sigFirst);
            if(transforms && file.sigFirst.normalized.size()==8)
                for(const auto& signature:file.sigFirst.normalized) add(signature);
        }
        if(!usesBucketIndex(threshold,points_.size())) return;
        heads_.assign(Blocks*Buckets+1,0); next_.resize(Blocks*points_.size());
        std::vector<unsigned> counts(Blocks*Buckets); std::array<unsigned,Blocks> largest{};
        for(int block=0;block<Blocks && !cancel_.load();++block) for(size_t i=0;i<points_.size();++i) {
            const auto bucket=block*Buckets+key(points_[i],block);
            largest[block]=std::max(largest[block],++counts[bucket]);
        }
        for(size_t bucket=0;bucket<counts.size();++bucket) heads_[bucket+1]=heads_[bucket]+int(counts[bucket]);
        auto cursor=heads_;
        // Contiguous postings and inline hashes avoid pointer chasing through
        // the much larger image evidence objects on every candidate visit.
        for(int block=0;block<Blocks && !cancel_.load();++block) for(size_t i=0;i<points_.size();++i) next_[cursor[block*Buckets+key(points_[i],block)]++]=int(i);
        for(int block=0;block<Blocks;++block) searchBlocks_.push_back(block);
        std::stable_sort(searchBlocks_.begin(),searchBlocks_.end(),[&](int a,int b) { return largest[a]<largest[b]; });
        // Any subset still satisfies the Hamming bound. Omit skewed blocks
        // (e.g. flat white backgrounds) instead of walking enormous buckets.
        while(searchBlocks_.size()>8 && largest[searchBlocks_.back()]>std::max<size_t>(64,points_.size()/32)) searchBlocks_.pop_back();
    }
    void query(const VideoFileInfo& file,double threshold,bool canonicalOnly,Scratch& scratch,std::vector<size_t>& result) const {
        result.clear();
        if(!file.frameLoaded || file.corrupted || file.previewFailed || !file.sigFirst.valid || file.sigFirst.lowVariance || points_.empty()) return;
        const unsigned maxDifference=maxHammingDifference(threshold);
        const bool linear=heads_.empty() || searchBlocks_.empty();
        const unsigned blockRadius=linear?4u:(maxDifference/unsigned(searchBlocks_.size()));
        if(scratch.visited.size()<points_.size()) scratch.visited.resize(points_.size(),0);
        if(scratch.returned.size()<positions_) scratch.returned.resize(positions_,0);
        scratch.queried.clear();
        auto find=[&](const Point& target) {
            const std::array<uint64_t,3> hashes{hash(target,0),hash(target,1),hash(target,2)};
            if(std::find(scratch.queried.begin(),scratch.queried.end(),hashes)!=scratch.queried.end()) return;
            scratch.queried.push_back(hashes);
            if(++scratch.stamp==0) {
                std::fill(scratch.visited.begin(),scratch.visited.end(),0);
                scratch.stamp=1;
            }
            const uint32_t stamp=scratch.stamp;
            auto visit=[&](size_t i) {
                const size_t pos=points_[i].position;
                if(scratch.returned[pos]) return;
                if(scratch.visited[i]==stamp) return;
                scratch.visited[i]=stamp;
                if(difference(target,points_[i])<=maxDifference) { scratch.returned[pos]=1; result.push_back(pos); }
            };
            // Large Hamming balls or unbuilt bucket tables use sequential scan over the compact Point array.
            if(linear || blockRadius>3 || (blockRadius==3 && points_.size()<=16384)) {
                for(size_t i=0;i<points_.size() && !cancel_.load(std::memory_order_relaxed);++i) visit(i);
                return;
            }
            // Sum(radius+1) > maxDifference is sufficient; extra probes add no recall.
            const unsigned wider=maxDifference%unsigned(searchBlocks_.size())+1;
            for(size_t order=0;order<searchBlocks_.size() && !cancel_.load(std::memory_order_relaxed);++order) {
                const int radius=int(blockRadius)-(order>=wider?1:0);
                if(radius<0) continue;
                const int block=searchBlocks_[order];
                const unsigned center=key(target,block);
                auto bucket=[&](unsigned value) {
                    const size_t slot=block*Buckets+value;
                    for(int offset=heads_[slot];offset<heads_[slot+1];++offset) {
                        if(cancel_.load(std::memory_order_relaxed)) return;
                        visit(size_t(next_[offset]));
                    }
                };
                bucket(center);
                if(radius>=1) for(int a=0;a<16;++a) {
                    bucket(center^(1u<<a));
                    if(radius>=2) for(int b=a+1;b<16;++b) {
                        bucket(center^(1u<<a)^(1u<<b));
                        if(radius>=3) for(int c=b+1;c<16;++c) bucket(center^(1u<<a)^(1u<<b)^(1u<<c));
                    }
                }
            }
        };
        auto signature=[&](const FrameSignature& s) {
            if(!s.valid || s.lowVariance) return;
            find(point(s,0,false));
            if(transforms_ && s.hasCenterCrop) find(point(s,0,true));
        };
        signature(file.sigFirst);
        if(transforms_ && file.sigFirst.normalized.size()==8) {
            if(canonicalOnly) signature(file.sigFirst.normalized[0]);
            else for(const auto& s:file.sigFirst.normalized) signature(s);
        }
        for(size_t pos:result) scratch.returned[pos]=0;
        std::sort(result.begin(),result.end());
    }
    std::vector<size_t> query(const VideoFileInfo& file,double threshold,bool canonicalOnly=false) const {
        Scratch scratch;
        std::vector<size_t> result;
        query(file,threshold,canonicalOnly,scratch,result);
        return result;
    }
};
