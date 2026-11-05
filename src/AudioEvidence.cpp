#include "AudioEvidence.h"
#include "FileOperations.h"
#include "platform/Platform.h"
#include <filesystem>
#include <numeric>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

namespace AudioEvidence {
namespace {
constexpr int rate=8000;
struct Reader {
    AVFormatContext* format=nullptr; AVCodecContext* codec=nullptr; SwrContext* swr=nullptr;
    AVPacket* packet=av_packet_alloc(); AVFrame* frame=av_frame_alloc();
    Platform::ReadLock lock; int index=-1,tracks=0; int64_t origin=0,duration=0;
    std::atomic<bool>& cancel; std::chrono::steady_clock::time_point deadline;
    explicit Reader(std::atomic<bool>& flag):cancel(flag) {}
    ~Reader() { swr_free(&swr); av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec); avformat_close_input(&format); }
    static int interrupted(void* opaque) { auto& r=*static_cast<Reader*>(opaque); return r.cancel.load() || std::chrono::steady_clock::now()>r.deadline; }
    bool open(const VideoFileInfo& file) {
        if(file.fullPath.find(L"://")!=std::wstring::npos || !file.identity.valid || !packet || !frame) return false;
        // Same open as ReadIdentity: long-path safe, and a link is locked as itself rather than followed.
        if(!lock.open(file.fullPath) || FileOperations::ReadIdentity(file.fullPath)!=file.identity) return false;
        deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        format=avformat_alloc_context(); if(!format) return false; format->interrupt_callback={interrupted,this};
        AVDictionary* options=nullptr; av_dict_set(&options,"protocol_whitelist","file",0);
        const auto path=Platform::FileNameBytes(file.fullPath);
        const int opened=avformat_open_input(&format,path.c_str(),nullptr,&options); av_dict_free(&options);
        if(opened<0 || avformat_find_stream_info(format,nullptr)<0) return false;
        for(unsigned i=0;i<format->nb_streams;++i) if(format->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO) ++tracks;
        if(!tracks) return true;
        index=av_find_best_stream(format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0); if(index<0) return false;
        auto stream=format->streams[index]; const auto decoder=avcodec_find_decoder(stream->codecpar->codec_id);
        duration=format->duration>0?av_rescale_q(format->duration,AV_TIME_BASE_Q,{1,10000000}):stream->duration>0?av_rescale_q(stream->duration,stream->time_base,{1,10000000}):0;
        if(!decoder || !(codec=avcodec_alloc_context3(decoder)) || avcodec_parameters_to_context(codec,stream->codecpar)<0) return false;
        codec->thread_count=2;
        if(avcodec_open2(codec,decoder,nullptr)<0 || codec->sample_rate<=0 || codec->sample_rate>768000 || codec->ch_layout.nb_channels<1 || codec->ch_layout.nb_channels>128) return false;
        AVChannelLayout mono=AV_CHANNEL_LAYOUT_MONO;
        if(swr_alloc_set_opts2(&swr,&mono,AV_SAMPLE_FMT_FLT,rate,&codec->ch_layout,codec->sample_fmt,codec->sample_rate,0,nullptr)<0 || swr_init(swr)<0) return false;
        if(format->start_time!=AV_NOPTS_VALUE) origin=av_rescale_q(format->start_time,AV_TIME_BASE_Q,{1,10000000});
        else if(stream->start_time!=AV_NOPTS_VALUE) origin=av_rescale_q(stream->start_time,stream->time_base,{1,10000000});
        return origin>-(INT64_MAX/4) && origin<INT64_MAX/4;
    }
    bool window(int64_t start,std::vector<float>& samples) {
        deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        const auto tb=format->streams[index]->time_base;
        if(av_seek_frame(format,index,av_rescale_q(start+origin,{1,10000000},tb),AVSEEK_FLAG_BACKWARD)<0) return false;
        avcodec_flush_buffers(codec); swr_close(swr); if(swr_init(swr)<0) return false;
        samples.assign(rate*6,0); std::vector<uint8_t> covered(samples.size(),0); int count=0;
        bool draining=false,positioned=false; int64_t cursor=0;
        while(!interrupted(this)) {
            int rc=avcodec_receive_frame(codec,frame);
            if(rc>=0) {
                const auto pts=frame->best_effort_timestamp;
                if(pts==AV_NOPTS_VALUE || frame->nb_samples<=0 || frame->nb_samples>codec->sample_rate*2) return false;
                const int64_t time=av_rescale_q(pts,tb,{1,10000000})-origin;
                if(time>=start+60000000) return count>=rate*5;
                const int capacity=swr_get_out_samples(swr,frame->nb_samples); if(capacity<=0 || capacity>rate*3) return false;
                std::vector<float> converted(capacity); uint8_t* out=reinterpret_cast<uint8_t*>(converted.data());
                const int64_t delay=swr_get_delay(swr,rate);
                const int n=swr_convert(swr,&out,capacity,const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples); if(n<0) return false;
                const int64_t stamped=av_rescale_q(time-start,{1,10000000},{1,rate})-delay;
                // Container timestamps can be rounded to milliseconds. Keep decoded PCM continuous.
                if(!positioned) { cursor=stamped; positioned=true; }
                if(std::abs(stamped-cursor)>rate/10) return false;
                const int64_t base=cursor; cursor+=n;
                for(int i=0;i<n;++i) if(base+i>=0 && base+i<int64_t(samples.size())) {
                    const auto at=size_t(base+i); samples[at]=converted[i]; if(!covered[at]) { covered[at]=1; ++count; }
                }
                av_frame_unref(frame); continue;
            }
            if(rc==AVERROR_EOF) return count>=rate*5;
            if(rc!=AVERROR(EAGAIN) || draining) return false;
            do { av_packet_unref(packet); rc=av_read_frame(format,packet); } while(rc>=0 && packet->stream_index!=index && !interrupted(this));
            if(rc==AVERROR_EOF) { draining=true; rc=avcodec_send_packet(codec,nullptr); }
            else if(rc>=0) rc=avcodec_send_packet(codec,packet);
            if(rc<0) return false;
        }
        return false;
    }
};
double correlation(const std::vector<float>& a,const std::vector<float>& b,int lag,int step) {
    double xy=0,xx=0,yy=0,x=0,y=0; unsigned n=0;
    for(int i=rate/4;i<int(a.size())-rate/4;i+=step) {
        const double u=a[i],v=b[i+lag]; xy+=u*v; xx+=u*u; yy+=v*v; x+=u; y+=v; ++n;
    }
    xx-=x*x/n; yy-=y*y/n; xy-=x*y/n;
    if(xx/n<1e-7 || yy/n<1e-7) return -1; // Silence cannot establish content equality.
    return std::clamp(std::abs(xy)/std::sqrt(xx*yy),0.0,1.0);
}
double bestCorrelation(const std::vector<float>& a,const std::vector<float>& b,std::atomic<bool>& cancel) {
    int bestLag=0; double best=-1;
    for(int lag=-rate/5;lag<=rate/5;++lag) {
        if(cancel.load()) return -1;
        double c=correlation(a,b,lag,4); if(c>best) { best=c; bestLag=lag; }
    }
    best=-1;
    for(int lag=bestLag-7;lag<=bestLag+7;++lag) best=std::max(best,correlation(a,b,lag,1));
    return best;
}
}
Result Compare(const VideoFileInfo& left,const VideoFileInfo& right,int64_t offset,std::atomic<bool>& cancel) {
    Result result; if(cancel.load()) { result.status=Status::Cancelled; return result; }
    Reader a(cancel),b(cancel);
    if(!a.open(left) || !b.open(right)) return result;
    result.leftTracks=a.tracks; result.rightTracks=b.tracks;
    if(!a.tracks && !b.tracks) { result.status=Status::NeitherHasAudio; return result; }
    if(!a.tracks || !b.tracks) { result.status=Status::OneHasAudio; return result; }
    if(offset<-(INT64_MAX/4) || offset>INT64_MAX/4) return result;
    if(a.duration<=0 || b.duration<=0 || a.duration>INT64_MAX/4 || b.duration>INT64_MAX/4) return result;
    const int64_t begin=std::max<int64_t>(0,-offset),end=std::min(a.duration,b.duration-offset);
    if(end-begin<80000000) { result.status=Status::Inconclusive; return result; }
    result.lowestCorrelation=1;
    for(int percent:{10,50,90}) {
        std::vector<float> x,y; const auto time=begin+(end-begin-60000000)*percent/100;
        if(!a.window(time,x) || !b.window(time+offset,y)) { result.status=cancel.load()?Status::Cancelled:Status::Failed; return result; }
        const double c=bestCorrelation(x,y,cancel); if(cancel.load()) { result.status=Status::Cancelled; return result; }
        if(c<0) { result.status=Status::Inconclusive; return result; }
        result.lowestCorrelation=std::min(result.lowestCorrelation,c); ++result.windows;
    }
    if(FileOperations::ReadIdentity(left.fullPath)!=left.identity || FileOperations::ReadIdentity(right.fullPath)!=right.identity) return Result{};
    // Conservative sampled waveform evidence; never a full-stream or language identity claim.
    result.status=result.lowestCorrelation>=0.90?Status::SimilarSamples:result.lowestCorrelation<0.50?Status::DifferentSamples:Status::Inconclusive;
    return result;
}
}
