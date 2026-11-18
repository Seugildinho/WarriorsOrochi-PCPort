#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MPEG.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {
unsigned callbacks{},mainCalls{};
uint32_t expectedAddress{},expectedLength{};
uint32_t expectedType{},expectedPrefix{};
bool correct{};
bool useOriginalCallback{};
void callback(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    ++callbacks;
    const auto data=getRegU32(ctx,5);
    if(data>0x2000000-32) {ctx->pc=0;return;}
    const auto address=runtime->memory().read32(data+8);
    uint32_t prefix{};
    if(address<=0x2000000-4) std::memcpy(&prefix,ram+address,4);
    correct=runtime->memory().read32(data)==expectedType && address==expectedAddress &&
        runtime->memory().read32(data+12)==expectedLength && address<=0x2000000-4 &&
        prefix==expectedPrefix;
    ctx->pc=useOriginalCallback?0x115008:0;
}
void wait(uint8_t *,R5900Context *ctx,PS2Runtime *runtime) {
    if(callbacks || ++mainCalls>1000) {ctx->pc=0;runtime->requestStop();return;}
    ctx->pc=0x100100;
}
bool test(uint32_t type,uint8_t substream,bool split,bool wrongId=false,bool original=false) {
    auto runtime=std::make_unique<PS2Runtime>();
    if(!runtime->memory().initialize())return false;
    runtime->configureGuestHeap(0x1000000,0x1f00000);
    ps2_stubs::resetMpegStubState();
    runtime->registerFunction(0x100100,wait);runtime->registerFunction(0x100104,callback);
    auto *ram=runtime->memory().getRDRAM();
    constexpr uint32_t movie=0x800000,ring=movie+0x1100,queue=movie+0x91210;
    if(original) {
        runtime->memory().write32(0x358208,movie);
        runtime->memory().write32(ring+0x8008,0x8000);
        runtime->memory().write32(queue+4,1);
        runtime->memory().write32(queue+0x34,0x20000);
        runtime->memory().write32(queue+0x40,0x2000);
        runtime->memory().write32(queue+0x48,0x123540);
    }
    R5900Context registration{};
    registration.r[4]=_mm_set_epi64x(0,0x8000);registration.r[5]=_mm_set_epi64x(0,type);
    registration.r[6]=_mm_set_epi64x(0,wrongId?1:0);
    registration.r[7]=_mm_set_epi64x(0,0x100104);
    registration.r[8]=_mm_set_epi64x(0,original?ring:0);
    ps2_stubs::sceMpegAddStrCallback(ram,&registration,runtime.get());
    const uint8_t payload[]={'S','S','h','d',1,2,3,4};
    std::vector<uint8_t> packet={0,0,1,0xbd,0,15,0x80,0,0,0xff,substream,0,0};
    packet.insert(packet.end(),std::begin(payload),std::end(payload));
    const unsigned cut=split?8:packet.size();
    const uint32_t input=original?ring:0x10000;
    std::memcpy(ram+input,packet.data(),cut);
    if(split)std::memcpy(ram+0x30000,packet.data()+cut,packet.size()-cut);
    expectedAddress=split?0x30001:input+9;expectedLength=12;expectedType=type;
    expectedPrefix=0xffu|(static_cast<uint32_t>(substream)<<8u);
    callbacks=mainCalls=0;correct=false;useOriginalCallback=original;
    auto feed=[&](uint32_t address,uint32_t size) {
        R5900Context ctx{};ctx.r[4]=_mm_set_epi64x(0,0x8000);
        ctx.r[5]=_mm_set_epi64x(0,address);ctx.r[6]=_mm_set_epi64x(0,size);
        ctx.r[28]=_mm_set_epi64x(0,0x360170);
        ps2_stubs::sceMpegDemuxPss(ram,&ctx,runtime.get());
    };
    R5900Context main{};main.pc=0x100100;
    main.r[29]=_mm_set_epi64x(0,0x9000);main.r[28]=_mm_set_epi64x(0,0x360170);
    runtime->eeScheduler().reset(ram,main);
    feed(input,cut);
    if(split)feed(0x30000,packet.size()-cut);
    runtime->eeScheduler().run();
    if(original) correct &= runtime->memory().read32(queue+0x30)==8 &&
        std::memcmp(ram+queue+8,payload,sizeof(payload))==0;
    return wrongId?callbacks==0:callbacks==1 && correct;
}
}
int runMpegAudioRegressionTests() {
    const bool pcm=test(3,0xa1,false),adpcm=test(4,0x90,false),split=test(3,0xa1,true),wrong=test(3,0xa1,false,true),original=test(3,0xa1,false,false,true);
    std::cout << "[MPEG audio regression] original SDK audio selectors 3 and 4 preserve their private headers: " << (pcm && adpcm?"PASS":"FAIL") << '\n';
    std::cout << "[MPEG audio regression] split PES preserves payload address/length and rejects a different stream ID: " << (split && wrong?"PASS":"FAIL") << '\n';
    std::cout << "[MPEG audio regression] original game callback copies only the audio header into its own queue: " << (original?"PASS":"FAIL") << '\n';
    return pcm && adpcm && split && wrong && original?0:1;
}
