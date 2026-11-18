#pragma once
#include <cstdint>
class PS2Runtime;
struct OrochiImagePerfSnapshot {
    uint64_t decodedFrames{};        // ipu2 frames completed (decoded or served from cache)
    uint64_t totalMicroseconds{};    // whole decodeActiveImage, including retiling
    uint64_t processMicroseconds{};  // time inside the decoder (libavcodec, or ffmpeg.exe in CLI mode)
    uint64_t cacheHits{};            // frames reusing the previous identical payload (field repeat)
};
bool installOrochiImageFunctions(PS2Runtime &runtime);
OrochiImagePerfSnapshot getOrochiImagePerfSnapshot();
