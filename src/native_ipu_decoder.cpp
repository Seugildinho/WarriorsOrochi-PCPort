#include "native_ipu_decoder.h"
#include <cstring>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace {
[[noreturn]] void fail(const char *what,int code=0) {
    std::string message=what;
    if(code<0) {
        char text[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(code,text,sizeof(text));
        message+=" (";message+=text;message+=")";
    }
    throw std::runtime_error(message);
}
}

struct OrochiIpuDecoder::State {
    AVCodecContext *codec{};
    AVFrame *frame{};
    AVPacket *packet{};
    SwsContext *sws{};
    uint32_t width{},height{};
    ~State() { close(); }
    void close() {
        sws_freeContext(sws);sws=nullptr;
        avcodec_free_context(&codec);
        av_frame_free(&frame);
        av_packet_free(&packet);
        width=height=0;
    }
    void open(uint32_t w,uint32_t h) {
        close();
        const AVCodec *decoder=avcodec_find_decoder(AV_CODEC_ID_IPU);
        if(!decoder) fail("libavcodec was built without the IPU decoder");
        codec=avcodec_alloc_context3(decoder);
        frame=av_frame_alloc();packet=av_packet_alloc();
        if(!codec||!frame||!packet) fail("could not allocate the IPU decoder");
        codec->width=int(w);codec->height=int(h);
        codec->thread_count=1; // intra-only stream: one packet in, one frame out
        const int result=avcodec_open2(codec,decoder,nullptr);
        if(result<0) fail("could not open the IPU decoder",result);
        width=w;height=h;
    }
};

OrochiIpuDecoder::OrochiIpuDecoder():state(std::make_unique<State>()) {}
OrochiIpuDecoder::~OrochiIpuDecoder()=default;

void OrochiIpuDecoder::decode(const uint8_t *payload,uint32_t bytes,uint32_t width,uint32_t height,
                              std::vector<uint8_t> &rgba) {
    auto &s=*state;
    if(!payload||!bytes||bytes>0x7fffffffu) fail("invalid IPU payload");
    if(!s.codec||s.width!=width||s.height!=height) s.open(width,height);
    av_packet_unref(s.packet);
    int result=av_new_packet(s.packet,int(bytes)); // zeroed input padding included
    if(result<0) fail("could not allocate the IPU packet",result);
    std::memcpy(s.packet->data,payload,bytes);
    result=avcodec_send_packet(s.codec,s.packet);
    av_packet_unref(s.packet);
    if(result<0) { s.close(); fail("IPU decoder rejected the frame",result); }
    result=avcodec_receive_frame(s.codec,s.frame);
    if(result<0) { s.close(); fail("IPU decoder produced no frame",result); }
    const auto *frame=s.frame;
    if(frame->width!=int(width)||frame->height!=int(height)) {
        av_frame_unref(s.frame);s.close();fail("IPU decoder returned unexpected dimensions");
    }
    // Same conversion as `ffmpeg -pix_fmt rgba` (fftools default sws flags: bicubic).
    s.sws=sws_getCachedContext(s.sws,int(width),int(height),AVPixelFormat(frame->format),
        int(width),int(height),AV_PIX_FMT_RGBA,SWS_BICUBIC,nullptr,nullptr,nullptr);
    if(!s.sws) { av_frame_unref(s.frame);fail("could not create the IPU colour converter"); }
    rgba.resize(size_t(width)*height*4u);
    uint8_t *planes[4]={rgba.data(),nullptr,nullptr,nullptr};
    int strides[4]={int(width*4u),0,0,0};
    const int rows=sws_scale(s.sws,frame->data,frame->linesize,0,int(height),planes,strides);
    av_frame_unref(s.frame);
    if(rows!=int(height)) fail("IPU colour conversion was incomplete");
}
