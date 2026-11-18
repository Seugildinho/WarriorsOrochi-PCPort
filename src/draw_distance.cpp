#include "draw_distance.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

// Read every time the game sets the limit; tests change it in a running game.
int32_t g_orochiCrowdTarget=0;

namespace {
// How many characters the game draws in full. Each frame it ranks the
// officers and soldiers on the field by distance (orochi_block_197648) and
// only the nearest N get a body; the rest show just their health bar until
// they come closer. FUN_001f2110 sets N in gp-0x7B2C: a per-stage value
// (at most 51), 30 with two players, 20 in one special mode. The ranking
// loop only compares against it, so a larger N needs no bigger tables; the
// pool holds 147 characters.
constexpr uint32_t limitPc=0x1f2110u;
constexpr uint32_t gp=0x360170u,limitAddress=gp-0x7B2Cu;
constexpr int32_t gameMaximum=51,poolSize=147;

PS2Runtime::RecompiledFunction limitOriginal{};

void setLimit(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    limitOriginal(ram,ctx,runtime);
    int32_t limit;
    std::memcpy(&limit,ram+limitAddress,4);
    // Scale the stage's own value, so busier stages stay busier.
    const int32_t raised=std::min(poolSize,limit*g_orochiCrowdTarget/gameMaximum);
    if(raised>limit) std::memcpy(ram+limitAddress,&raised,4);
}
}

bool installOrochiDrawDistance(PS2Runtime &runtime) {
    const char *value=std::getenv("OROCHI_CROWD");
    if(!value) return true;
    g_orochiCrowdTarget=std::clamp(std::atoi(value),0,poolSize);
    if(g_orochiCrowdTarget<=gameMaximum) return true;
    std::cout << "[Orochi:draw] characters drawn in full: up to " << g_orochiCrowdTarget << " (game: " << gameMaximum << ")\n";
    limitOriginal=runtime.lookupFunction(limitPc);
    return limitOriginal && runtime.replaceFunction(limitPc,setLimit);
}
