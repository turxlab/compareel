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
    ImageCandidates(const std::vector<VideoFileInfo>& files,const std::vector<int>& candidates,
                    bool transforms,std::atomic<bool>& cancel):cancel_(cancel),transforms_(transforms) {
        positions_=candidates.size();
        for(size_t pos=0;pos<candidates.size() && !cancel_.load();++pos) {
            const auto& file=files[candidates[pos]];
            if(!file.frameLoaded || file.corrupted || file.imageKind==ImageEvidenceKind::AnimatedImage || file.imageKind==ImageEvidenceKind::MultiPageDocument) continue;
            auto add=[&](const FrameSignature& signature) {
                if(!signature.valid || signature.lowVariance) return;
                points_.push_back(point(signature,pos,false));
                if(transforms && signature.hasCenterCrop) points_.push_back(point(signature,pos,true));
            };
            add(file.sigFirst);
            if(transforms && file.sigFirst.normalized.size()==8)
                for(const auto& signature:file.sigFirst.normalized) add(signature);
        }
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
    std::vector<size_t> query(const VideoFileInfo& file,double threshold,bool canonicalOnly=false) const {
        std::vector<size_t> result;
        if(!file.frameLoaded || !file.sigFirst.valid || file.sigFirst.lowVariance) return result;
        const unsigned maxDifference=static_cast<unsigned>(std::max(0.0,std::floor(192.0*(100.0-threshold)/65.0+1e-7)));
        const unsigned blockRadius=maxDifference/unsigned(searchBlocks_.size());
        std::vector<uint8_t> visited(points_.size()); uint8_t stamp=0;
        std::vector<uint8_t> returned(positions_);
        std::vector<std::array<uint64_t,3>> queried;
        auto find=[&](const Point& target) {
            const std::array<uint64_t,3> hashes{hash(target,0),hash(target,1),hash(target,2)};
            if(std::find(queried.begin(),queried.end(),hashes)!=queried.end()) return;
            queried.push_back(hashes);
            ++stamp;
            auto visit=[&](size_t i) {
                if(returned[points_[i].position]) return;
                if(visited[i]==stamp) return;
                visited[i]=stamp;
                if(difference(target,points_[i])<=maxDifference && !returned[points_[i].position]) { returned[points_[i].position]=1; result.push_back(points_[i].position); }
            };
            // shortcut: thresholds below 90 use exhaustive search; a large Hamming
            // ball costs more bucket probes than the original comparison loop.
            if(blockRadius>3) {
                for(size_t i=0;i<points_.size() && !cancel_.load();++i) visit(i);
                return;
            }
            // Sum(radius+1) > maxDifference is sufficient; extra probes add no recall.
            const unsigned wider=maxDifference%unsigned(searchBlocks_.size())+1;
            for(size_t order=0;order<searchBlocks_.size() && !cancel_.load();++order) {
                const int radius=int(blockRadius)-(order>=wider?1:0);
                if(radius<0) continue;
                const int block=searchBlocks_[order];
                const unsigned center=key(target,block);
                auto bucket=[&](unsigned value) {
                    const size_t slot=block*Buckets+value;
                    for(int offset=heads_[slot];offset<heads_[slot+1];++offset) {
                        if(cancel_.load()) return;
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
        std::sort(result.begin(),result.end()); result.erase(std::unique(result.begin(),result.end()),result.end());
        return result;
    }
};
