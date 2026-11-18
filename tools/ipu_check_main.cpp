// orochi_ipu_check: validates the in-process IPU decoder (src/native_ipu_decoder.cpp)
// against a reference RGBA frame produced by the FFmpeg CLI path previously used
// by native_image.cpp:
//   ffmpeg -i frame.ipu -frames:v 1 -pix_fmt rgba -f rawvideo frame.rgba
// Usage: orochi_ipu_check <frame.ipu with ipum header> <reference.rgba> [iterations]
// The sample is game-derived; keep it under local/ (never commit it).
#include "native_ipu_decoder.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

static std::vector<uint8_t> readAll(const char *path) {
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}

int main(int argc,char **argv) {
    if(argc<3) { std::cerr<<"usage: orochi_ipu_check <frame.ipu> <reference.rgba> [iterations]\n";return 2; }
    const auto input=readAll(argv[1]),reference=readAll(argv[2]);
    const int iterations=argc>3?std::max(1,std::atoi(argv[3])):200;
    uint32_t header[4]{};
    if(input.size()<20) { std::cerr<<"[ipu-check] FAIL input too small\n";return 1; }
    std::memcpy(header,input.data(),16);
    const uint32_t width=header[2]&0xffffu,height=header[2]>>16;
    if(header[0]!=0x6d757069u||header[1]!=input.size()||!width||!height) {
        std::cerr<<"[ipu-check] FAIL invalid ipum header\n";return 1;
    }
    try {
        OrochiIpuDecoder decoder;std::vector<uint8_t> rgba;
        decoder.decode(input.data()+16,uint32_t(input.size()-16),width,height,rgba);
        size_t diff=0;int maxDelta=0;
        if(rgba.size()!=reference.size()) {
            std::cerr<<"[ipu-check] FAIL size "<<rgba.size()<<" != reference "<<reference.size()<<"\n";return 1;
        }
        for(size_t i=0;i<rgba.size();i++) if(rgba[i]!=reference[i]) {
            ++diff;maxDelta=std::max(maxDelta,std::abs(int(rgba[i])-int(reference[i])));
        }
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<iterations;i++) decoder.decode(input.data()+16,uint32_t(input.size()-16),width,height,rgba);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::printf("[ipu-check] %s size=%ux%u payload=%zu diffBytes=%zu maxDelta=%d iterations=%d avgMs=%.3f\n",
            diff?"FAIL":"PASS",width,height,input.size()-16,diff,maxDelta,iterations,ms/iterations);
        return diff?1:0;
    } catch(const std::exception &error) {
        std::cerr<<"[ipu-check] FAIL "<<error.what()<<"\n";return 1;
    }
}
