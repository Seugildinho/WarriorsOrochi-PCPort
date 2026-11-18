#pragma once
// In-process decoder for one PS2 IPU intra frame (ipu2 entry) using libavcodec.
// Replaces the per-frame external ffmpeg.exe process formerly spawned by
// native_image.cpp; same decoder (AV_CODEC_ID_IPU) and the same YUV->RGBA
// conversion flags that the FFmpeg CLI applies (swscale, SWS_BICUBIC).
#include <cstdint>
#include <memory>
#include <vector>

class OrochiIpuDecoder {
public:
    OrochiIpuDecoder();
    ~OrochiIpuDecoder();
    OrochiIpuDecoder(const OrochiIpuDecoder &)=delete;
    OrochiIpuDecoder &operator=(const OrochiIpuDecoder &)=delete;
    // payload: frame bytes up to and including the 00 00 01 b0 terminator
    // (exactly what FFmpeg's ipu demuxer/parser hands to the decoder).
    // Output: linear RGBA, width*height*4 bytes. Throws std::runtime_error.
    void decode(const uint8_t *payload,uint32_t bytes,uint32_t width,uint32_t height,
                std::vector<uint8_t> &rgba);
private:
    struct State;
    std::unique_ptr<State> state;
};
