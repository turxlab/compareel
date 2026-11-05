#include "FFmpegReader.h"
#include "PerceptualHash.h"
#include "platform/Platform.h"
#include <chrono>
#include <filesystem>
#include <set>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>
}

namespace {
std::atomic<bool> gpuEnabled=false;
// Each platform's native decoding API; frames are copied back to memory before filtering.
#ifdef _WIN32
constexpr auto hardwareDevice=AV_HWDEVICE_TYPE_D3D11VA; constexpr auto hardwareFormat=AV_PIX_FMT_D3D11;
#elif defined(__APPLE__)
constexpr auto hardwareDevice=AV_HWDEVICE_TYPE_VIDEOTOOLBOX; constexpr auto hardwareFormat=AV_PIX_FMT_VIDEOTOOLBOX;
#else
constexpr auto hardwareDevice=AV_HWDEVICE_TYPE_VAAPI; constexpr auto hardwareFormat=AV_PIX_FMT_VAAPI;
#endif
}
struct FFmpegReader::State {
    std::wstring path; bool withAudio=false,gpu=false,hardwareDecoded=false,cpuFallback=false;
    int64_t lastOutput=-1;
    int64_t decodedVideo=-1; // time of the last video frame the decoder produced since the last seek
    AVFrame* downloaded=av_frame_alloc();
    AVFormatContext* format=nullptr;
    AVCodecContext *video=nullptr,*audio=nullptr;
    AVPacket* packet=av_packet_alloc(); AVFrame* frame=av_frame_alloc(); AVFrame* filtered=av_frame_alloc();
    AVFilterGraph* graph=nullptr; AVFilterContext *source=nullptr,*sink=nullptr;
    SwrContext* resample=nullptr;
    int videoIndex=-1,audioIndex=-1,pending=-1;
    bool eof=false,flushed=false,failed=false,videoDone=false,audioDone=false;
    int rotation=0; bool hdr=false;
    int64_t origin=0; std::atomic<bool>* cancel=nullptr;
    std::chrono::steady_clock::time_point deadline;
    ~State() {
        av_frame_free(&downloaded);
        swr_free(&resample); avfilter_graph_free(&graph); av_frame_free(&filtered); av_frame_free(&frame);
        av_packet_free(&packet); avcodec_free_context(&audio); avcodec_free_context(&video); avformat_close_input(&format);
    }
    static int interrupted(void* opaque) {
        const auto& s=*static_cast<State*>(opaque);
        return (s.cancel && s.cancel->load()) || std::chrono::steady_clock::now()>s.deadline;
    }
    void budget() { deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15); }
    bool decoder(int index,AVCodecContext*& context) {
        auto stream=format->streams[index]; const auto codec=avcodec_find_decoder(stream->codecpar->codec_id);
        if(!codec || !(context=avcodec_alloc_context3(codec))) return false;
        context->thread_count=2;
        if(avcodec_parameters_to_context(context,stream->codecpar)<0) return false;
        if(gpu && stream->codecpar->codec_type==AVMEDIA_TYPE_VIDEO) {
            bool supported=false;
            for(int i=0;const auto config=avcodec_get_hw_config(codec,i);++i)
                if(config->device_type==hardwareDevice && (config->methods&AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) { supported=true; break; }
            if(supported && av_hwdevice_ctx_create(&context->hw_device_ctx,hardwareDevice,nullptr,nullptr,0)>=0) {
                context->get_format=[](AVCodecContext* ctx,const AVPixelFormat* formats) {
                    for(auto f=formats;*f!=AV_PIX_FMT_NONE;++f) if(*f==hardwareFormat) return *f;
                    return avcodec_default_get_format(ctx,formats);
                };
                if(avcodec_open2(context,codec,nullptr)>=0) return true;
                avcodec_free_context(&context); context=avcodec_alloc_context3(codec);
                if(!context || avcodec_parameters_to_context(context,stream->codecpar)<0) return false;
                context->thread_count=2; cpuFallback=true;
            } else cpuFallback=true;
            if(!supported) cpuFallback=true;
        }
        return avcodec_open2(context,codec,nullptr)>=0;
    }
    bool filters() {
        hdr=frame->color_trc==AVCOL_TRC_ARIB_STD_B67 || frame->color_trc==AVCOL_TRC_SMPTE2084 || hdr;
        graph=avfilter_graph_alloc(); if(!graph) return false;
        graph->nb_threads=2;
        auto tb=format->streams[videoIndex]->time_base;
        char args[256]; snprintf(args,sizeof(args),"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",frame->width,frame->height,frame->format,tb.num,tb.den,std::max(1,frame->sample_aspect_ratio.num),std::max(1,frame->sample_aspect_ratio.den));
        if(avfilter_graph_create_filter(&source,avfilter_get_by_name("buffer"),"input",args,nullptr,graph)<0 ||
           avfilter_graph_create_filter(&sink,avfilter_get_by_name("buffersink"),"output",nullptr,nullptr,graph)<0) return false;
        auto parameters=av_buffersrc_parameters_alloc(); if(!parameters) return false;
        parameters->format=frame->format; parameters->width=frame->width; parameters->height=frame->height;
        parameters->time_base=tb; parameters->sample_aspect_ratio=frame->sample_aspect_ratio;
        parameters->color_space=frame->colorspace; parameters->color_range=frame->color_range;
        const auto configured=av_buffersrc_parameters_set(source,parameters); av_free(parameters); if(configured<0) return false;
        std::string chain;
        // Tone mapping needs zscale (libzimg), which some system FFmpeg builds lack; without it HDR video still
        // decodes and compares, only with flatter colors, instead of failing to open at all.
        if(hdr && avfilter_get_by_name("zscale") && avfilter_get_by_name("tonemap")) chain="zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,tonemap=tonemap=mobius:desat=2,zscale=t=bt709:m=bt709:r=full,";
        if(rotation==90) chain+="transpose=clock,";
        else if(rotation==270) chain+="transpose=cclock,";
        else if(rotation==180) chain+="hflip,vflip,";
        chain+="format=bgra";
        AVFilterInOut *in=avfilter_inout_alloc(),*out=avfilter_inout_alloc();
        if(!in || !out) { avfilter_inout_free(&in); avfilter_inout_free(&out); return false; }
        out->name=av_strdup("in"); out->filter_ctx=source; out->pad_idx=0;
        in->name=av_strdup("out"); in->filter_ctx=sink; in->pad_idx=0;
        int result=avfilter_graph_parse_ptr(graph,chain.c_str(),&in,&out,nullptr);
        avfilter_inout_free(&in); avfilter_inout_free(&out);
        return result>=0 && avfilter_graph_config(graph,nullptr)>=0;
    }
    bool render(Sample& sample) {
        if(frame->format==hardwareFormat) {
            if(!downloaded) return false;
            av_frame_unref(downloaded);
            if(av_hwframe_transfer_data(downloaded,frame,0)<0 || av_frame_copy_props(downloaded,frame)<0) return false;
            std::swap(frame,downloaded); av_frame_unref(downloaded); hardwareDecoded=true;
        }
        if(!graph && !filters()) return false;
        if(av_buffersrc_add_frame_flags(source,frame,AV_BUFFERSRC_FLAG_KEEP_REF)<0 || av_buffersink_get_frame(sink,filtered)<0) return false;
        sample.width=filtered->width; sample.height=filtered->height;
        if(uint64_t(sample.width)*sample.height>67108864) return false;
        sample.bytes.resize(size_t(sample.width)*sample.height*4);
        for(uint32_t y=0;y<sample.height;++y) memcpy(sample.bytes.data()+size_t(y)*sample.width*4,filtered->data[0]+ptrdiff_t(y)*filtered->linesize[0],size_t(sample.width)*4);
        av_frame_unref(filtered); return true;
    }
};
FFmpegReader::FFmpegReader():state_(std::make_unique<State>()) {}
FFmpegReader::~FFmpegReader()=default;
void FFmpegReader::SetGpuEnabled(bool enabled) { gpuEnabled.store(enabled); }
bool FFmpegReader::GpuEnabled() { return gpuEnabled.load(); }
bool FFmpegReader::HardwareDecoded() const { return state_->hardwareDecoded; }
bool FFmpegReader::CpuFallbackUsed() const { return state_->cpuFallback; }
bool FFmpegReader::Open(const std::wstring& path,std::atomic<bool>* cancel,bool audio,DecodeMode mode) {
    state_=std::make_unique<State>(); auto& s=*state_; s.cancel=cancel; s.budget();
    s.path=path; s.withAudio=audio; s.gpu=mode==DecodeMode::PreferGpu || (mode==DecodeMode::Configured && gpuEnabled.load());
    // Only ordinary local files; demuxers cannot follow network/playlist protocols.
    if(path.find(L"://")!=std::wstring::npos || !s.packet || !s.frame || !s.filtered) return false;
    s.format=avformat_alloc_context(); if(!s.format) return false;
    s.format->interrupt_callback={State::interrupted,&s};
    AVDictionary* options=nullptr; av_dict_set(&options,"protocol_whitelist","file",0);
    const auto bytes=Platform::FileNameBytes(path);
    int result=avformat_open_input(&s.format,bytes.c_str(),nullptr,&options); av_dict_free(&options);
    if(result<0 || avformat_find_stream_info(s.format,nullptr)<0) return false;
    s.videoIndex=av_find_best_stream(s.format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    if(s.videoIndex<0 || !s.decoder(s.videoIndex,s.video)) return false;
    if(s.video->width<=0 || s.video->height<=0 || uint64_t(s.video->width)*s.video->height>67108864) return false;
    auto stream=s.format->streams[s.videoIndex];
    if(s.format->start_time!=AV_NOPTS_VALUE) s.origin=av_rescale_q(s.format->start_time,AV_TIME_BASE_Q,{1,10000000});
    else if(stream->start_time!=AV_NOPTS_VALUE) s.origin=av_rescale_q(stream->start_time,stream->time_base,{1,10000000});
    const auto matrix=av_packet_side_data_get(stream->codecpar->coded_side_data,stream->codecpar->nb_coded_side_data,AV_PKT_DATA_DISPLAYMATRIX);
    if(matrix && matrix->size>=9*sizeof(int32_t)) {
        const auto angle=av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix->data));
        if(std::isfinite(angle)) s.rotation=(int(std::lround(-angle/90))*90%360+360)%360;
    }
    s.hdr=s.video->color_trc==AVCOL_TRC_ARIB_STD_B67 || s.video->color_trc==AVCOL_TRC_SMPTE2084;
    if(audio) {
        const int index=av_find_best_stream(s.format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
        if(index>=0) {
            if(!s.decoder(index,s.audio)) return false;
            if(s.audio->sample_rate<=0 || s.audio->sample_rate>768000 || s.audio->ch_layout.nb_channels<1 || s.audio->ch_layout.nb_channels>128) return false;
            s.audioIndex=index; AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
            if(swr_alloc_set_opts2(&s.resample,&stereo,AV_SAMPLE_FMT_S16,48000,&s.audio->ch_layout,s.audio->sample_fmt,s.audio->sample_rate,0,nullptr)<0 || swr_init(s.resample)<0) return false;
        }
    }
    return true;
}
bool FFmpegReader::Metadata(VideoFileInfo& file) const {
    const auto& s=*state_; if(!s.format || !s.video) return false;
    file.width=s.video->width; file.height=s.video->height;
    if(s.rotation==90 || s.rotation==270) std::swap(file.width,file.height);
    file.duration100ns=s.format->duration>0?av_rescale_q(s.format->duration,AV_TIME_BASE_Q,{1,10000000}):0;
    auto stream=s.format->streams[s.videoIndex];
    if(file.duration100ns<=0 && stream->duration>0) file.duration100ns=av_rescale_q(stream->duration,stream->time_base,{1,10000000});
    const auto rate=av_guess_frame_rate(s.format,stream,nullptr); file.fps=rate.den?av_q2d(rate):0;
    const auto codec=avcodec_get_name(s.video->codec_id); file.codec=std::wstring(codec,codec+strlen(codec));
    file.videoBitrate=uint32_t(std::clamp<int64_t>(stream->codecpar->bit_rate,0,UINT32_MAX));
    int audio=av_find_best_stream(s.format,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0); file.audioChannels=0;
    if(audio>=0) { const auto p=s.format->streams[audio]->codecpar; file.audioChannels=p->ch_layout.nb_channels; file.audioSampleRate=p->sample_rate; const auto name=avcodec_get_name(p->codec_id); file.audioCodec=std::wstring(name,name+strlen(name)); }
    if(file.duration100ns<0 || file.duration100ns>INT64_MAX/100 || s.origin<-(INT64_MAX/4) || s.origin>INT64_MAX/4) return false;
    file.metadataLoaded=true; file.corrupted=false; file.error=S_OK;
    return true;
}
bool FFmpegReader::HasAudio() const { return state_->audioIndex>=0; }
bool FFmpegReader::Failed() const { return state_->failed; }
bool FFmpegReader::Next(Sample& sample) {
    if(Next(sample,true)) { state_->lastOutput=sample.time; return true; }
    const auto last=state_->lastOutput;
    if(!state_->failed || !fallback(std::max<int64_t>(0,last))) return false;
    while(Next(sample,true)) if(sample.time>last) { state_->lastOutput=sample.time; return true; }
    return false;
}
bool FFmpegReader::fallback(int64_t target) {
    if(!state_->gpu || (state_->cancel && state_->cancel->load())) return false;
    const auto path=state_->path; const auto cancel=state_->cancel; const bool audio=state_->withAudio;
    if(!Open(path,cancel,audio,DecodeMode::Cpu)) return false;
    state_->cpuFallback=true;
    return Seek(target);
}
bool FFmpegReader::Next(Sample& sample,bool render) {
    auto& s=*state_; s.budget();
    av_frame_unref(s.frame);
    while(!State::interrupted(&s)) {
        if(s.pending>=0) {
            auto decoder=s.pending==s.videoIndex?s.video:s.audio;
            const int result=avcodec_receive_frame(decoder,s.frame);
            if(result>=0) {
                const auto pts=s.frame->best_effort_timestamp;
                if(pts==AV_NOPTS_VALUE) { s.failed=true; return false; }
                sample={}; sample.video=s.pending==s.videoIndex;
                const auto timestamp=av_rescale_q(pts,s.format->streams[s.pending]->time_base,{1,10000000});
                if(timestamp<-(INT64_MAX/4) || timestamp>INT64_MAX/4) { s.failed=true; return false; }
                sample.time=std::max<int64_t>(0,timestamp-s.origin);
                if(sample.video) {
                    s.decodedVideo=sample.time;
                    if(render && !s.render(sample)) { s.failed=true; return false; }
                } else if(render) {
                    const int capacity=int(av_rescale_rnd(swr_get_delay(s.resample,s.audio->sample_rate)+s.frame->nb_samples,48000,s.audio->sample_rate,AV_ROUND_UP));
                    if(capacity<0 || capacity>480000) { s.failed=true; return false; }
                    sample.bytes.resize(size_t(capacity)*4); uint8_t* output=sample.bytes.data();
                    const int count=swr_convert(s.resample,&output,capacity,const_cast<const uint8_t**>(s.frame->extended_data),s.frame->nb_samples);
                    if(count<0) { s.failed=true; return false; } sample.bytes.resize(size_t(count)*4);
                }
                return true;
            }
            if(result!=AVERROR(EAGAIN) && result!=AVERROR_EOF) { fprintf(stderr,"FFmpeg receive error=%d\n",result); s.failed=true; return false; }
            if(s.eof) { if(s.pending==s.videoIndex) s.videoDone=true; else s.audioDone=true; }
            s.pending=-1;
        }
        if(s.eof) {
            if(!s.flushed) { avcodec_send_packet(s.video,nullptr); if(s.audio) avcodec_send_packet(s.audio,nullptr); s.flushed=true; }
            if(!s.videoDone) s.pending=s.videoIndex;
            else if(s.audio && !s.audioDone) s.pending=s.audioIndex;
            else return false;
            continue;
        }
        const int read=av_read_frame(s.format,s.packet);
        if(read==AVERROR_EOF) { s.eof=true; continue; }
        if(read<0) { s.failed=true; return false; }
        auto decoder=s.packet->stream_index==s.videoIndex?s.video:s.packet->stream_index==s.audioIndex?s.audio:nullptr;
        if(decoder) {
            const int sent=avcodec_send_packet(decoder,s.packet);
            if(sent<0) { fprintf(stderr,"FFmpeg send error=%d stream=%d\n",sent,s.packet->stream_index); av_packet_unref(s.packet); s.failed=true; return false; }
            s.pending=s.packet->stream_index;
        }
        av_packet_unref(s.packet);
    }
    return false;
}
bool FFmpegReader::Seek(int64_t time) {
    auto& s=*state_; s.budget();
    if(time<0 || time>INT64_MAX/4) return false;
    const auto target=av_rescale_q(time+s.origin,{1,10000000},s.format->streams[s.videoIndex]->time_base);
    if(av_seek_frame(s.format,s.videoIndex,target,AVSEEK_FLAG_BACKWARD)<0) return false;
    avcodec_flush_buffers(s.video); if(s.audio) avcodec_flush_buffers(s.audio);
    if(s.resample) { swr_close(s.resample); if(swr_init(s.resample)<0) return false; }
    avfilter_graph_free(&s.graph); s.source=s.sink=nullptr;
    s.pending=-1; s.lastOutput=time-1; s.decodedVideo=-1; s.eof=s.flushed=s.failed=s.videoDone=s.audioDone=false; return true;
}
bool FFmpegReader::VideoAt(int64_t target,Sample& sample,bool previous,bool exact,const std::function<bool()>& superseded) {
    if(VideoAtImpl(target,sample,previous,exact,superseded)) return true;
    if((superseded && superseded()) || !fallback(target)) return false;
    return VideoAtImpl(target,sample,previous,exact,superseded);
}
bool FFmpegReader::VideoAtImpl(int64_t target,Sample& sample,bool previous,bool exact,const std::function<bool()>& superseded) {
    auto& s=*state_;
    // A scrub forward within reach goes on from the frame decoded last instead of back to the keyframe, so every
    // step of a drag moves the picture on, even when keyframes are seconds apart.
    const bool resume=!exact && !previous && s.decodedVideo>=0 && target>s.decodedVideo && target-s.decodedVideo<=100000000 && !s.eof && !s.failed;
    if(!resume && !Seek(std::max<int64_t>(0,target-(previous?10000000:0)))) return false;
    auto freeFrame=[](AVFrame* frame) { av_frame_free(&frame); };
    std::unique_ptr<AVFrame,decltype(freeFrame)> held(av_frame_alloc(),freeFrame);
    if(!held) return false;
    Sample last,current; bool have=false;
    // A scrub (not exact) decodes from the keyframe toward the target like an exact seek, but shows the frame it has
    // reached once a newer position is waiting or about two display frames have passed. Showing the keyframe at once
    // would freeze the picture on files with few keyframes while the slider is dragged.
    const auto started=std::chrono::steady_clock::now();
    auto settle=[&] { return !exact && ((superseded && superseded()) || std::chrono::steady_clock::now()-started>std::chrono::milliseconds(40)); };
    while(!(exact && superseded && superseded()) && Next(current,false)) if(current.video) {
        // Decode preroll for exact seeks, but only tone-map/copy the frame we display.
        if(current.time>=target || settle()) {
            if(previous && have) { av_frame_unref(s.frame); av_frame_move_ref(s.frame,held.get()); current=std::move(last); }
            if(exact && superseded && superseded()) return false;
            sample=std::move(current); return s.render(sample);
        }
        av_frame_unref(held.get());
        if(av_frame_ref(held.get(),s.frame)<0) return false;
        last=std::move(current); have=true;
    }
    if(have && !Failed() && !(superseded && superseded())) {
        av_frame_unref(s.frame); av_frame_move_ref(s.frame,held.get()); sample=std::move(last); return s.render(sample);
    }
    return false;
}
bool FFmpegReader::SampleTimeline(int64_t step100ns, size_t maxSamples, std::vector<FrameSignature>& outSamples,int orientations) {
    if(SampleTimelineImpl(step100ns,maxSamples,outSamples,orientations)) return true;
    if(!fallback(0)) return false;
    return SampleTimelineImpl(step100ns,maxSamples,outSamples,orientations);
}
bool FFmpegReader::SampleTimelineImpl(int64_t step100ns, size_t maxSamples, std::vector<FrameSignature>& outSamples,int orientations) {
    outSamples.clear();
    if(step100ns<=0 || maxSamples==0) return false;
    auto& s=*state_; if(!s.format || !s.video) return false;
    if(!Seek(0)) return false;
    Sample sample;
    int64_t nextTarget=0;
    while(!State::interrupted(&s) && outSamples.size()<maxSamples && Next(sample,false)) {
        if(!sample.video || sample.time<nextTarget) continue;
        if(!s.render(sample)) return false;
        auto sig=PerceptualHash::ComputeSignatureFromBGRA(sample.bytes.data(),sample.width,sample.height,sample.width*4);
        sig.timestamp100ns=sample.time;
        if(orientations>0) PerceptualHash::AddNormalizedOrientations(sample.bytes.data(),sample.width,sample.height,sample.width*4,sig,orientations);
        outSamples.push_back(std::move(sig));
        nextTarget=((sample.time/step100ns)+1)*step100ns;
        if(outSamples.size()<maxSamples && nextTarget-sample.time>=20000000LL) {
            const auto deadline=s.deadline;
            if(!Seek(nextTarget)) break;
            s.deadline=deadline;
        }
    }
    return !State::interrupted(&s) && !Failed() && !outSamples.empty();
}
static bool ReadFFmpegSamplesImpl(VideoFileInfo& file,bool sampleVideo,std::atomic<bool>* cancel) {
    FFmpegReader reader; if(!reader.Open(file.fullPath,cancel) || !reader.Metadata(file)) return false;
    FFmpegReader::Sample sample;
    auto read=[&](int64_t time,FrameSignature& signature,bool orientations) {
        if(!reader.VideoAt(time,sample)) return false;
        signature=PerceptualHash::ComputeSignatureFromBGRA(sample.bytes.data(),sample.width,sample.height,sample.width*4); signature.timestamp100ns=sample.time;
        if(orientations) PerceptualHash::AddNormalizedOrientations(sample.bytes.data(),sample.width,sample.height,sample.width*4,signature);
        PerceptualHash::GenerateThumbnailBGRA(sample.bytes.data(),sample.width,sample.height,sample.width*4,file.thumbBGRA); return signature.valid;
    };
    file.samples.clear(); if(!read(0,file.sigFirst,false)) return false;
    if(file.sigFirst.lowVariance && file.duration100ns>3000000) read(std::min<int64_t>(5000000,file.duration100ns/3),file.sigAlt,false);
    std::set<int64_t> timestamps;
    auto thumbnail=file.thumbBGRA;
    if(sampleVideo) for(int position:{10,30,50,70,90}) {
        FrameSignature signature;
        if(!read(file.duration100ns*position/100,signature,true)) return false;
        if(signature.lowVariance || !timestamps.insert(signature.timestamp100ns).second) signature.valid=false;
        if(position==50 && signature.valid) thumbnail=file.thumbBGRA;
        file.samples.push_back(std::move(signature));
    }
    file.thumbBGRA=std::move(thumbnail);
    file.frameLoaded=true; file.corrupted=false; file.error=S_OK; return true;
}
bool ReadFFmpegSamples(VideoFileInfo& file,bool sampleVideo,std::atomic<bool>* cancel) {
    if(cancel && cancel->load()) return false;
    auto result=file;
    if(!ReadFFmpegSamplesImpl(result,sampleVideo,cancel) || (cancel && cancel->load())) {
        file.samples.clear(); file.frameLoaded=false; file.sigFirst={}; file.sigAlt={}; file.thumbBGRA.clear(); return false;
    }
    file=std::move(result); return true;
}

bool ReadFFmpegSubtitleTracks(const std::wstring& path,std::vector<std::string>& tracks,std::atomic<bool>* cancel) {
    tracks.clear();
    if(path.find(L"://")!=std::wstring::npos) return false;
    AVFormatContext* format=avformat_alloc_context(); if(!format) return false;
    // Same 15 s budget as the main reader, so a stalled network share cannot hold a worker indefinitely.
    struct Budget { std::atomic<bool>* cancel; std::chrono::steady_clock::time_point deadline; } budget{cancel,std::chrono::steady_clock::now()+std::chrono::seconds(15)};
    format->interrupt_callback={[](void* data)->int { auto& b=*static_cast<Budget*>(data); return (b.cancel && b.cancel->load()) || std::chrono::steady_clock::now()>b.deadline; },&budget};
    AVDictionary* options=nullptr; av_dict_set(&options,"protocol_whitelist","file",0);
    const auto bytes=Platform::FileNameBytes(path);
    int result=avformat_open_input(&format,bytes.c_str(),nullptr,&options); av_dict_free(&options);
    if(result<0) return false; // avformat_open_input frees the context on failure
    struct Close { AVFormatContext** format; ~Close() { avformat_close_input(format); } } close{&format};
    // Stream info is needed for containers such as MPEG-TS that announce subtitle tracks late.
    if(avformat_find_stream_info(format,nullptr)<0 || (cancel && cancel->load()) || format->nb_streams>4096) return false;
    for(unsigned i=0;i<format->nb_streams;++i) {
        const auto stream=format->streams[i];
        if(stream->codecpar->codec_type!=AVMEDIA_TYPE_SUBTITLE) continue;
        std::string track=avcodec_get_name(stream->codecpar->codec_id);
        if(const auto language=av_dict_get(stream->metadata,"language",nullptr,0); language && language->value && *language->value)
            track+=std::string(" (")+std::string(language->value).substr(0,64)+")";
        tracks.push_back(std::move(track));
        if(tracks.size()>256) return false;
    }
    return true;
}
