#include "native_pad.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Pad.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>

namespace {
constexpr uint32_t orochiPadReadPc=0x0018eb70u;
PS2Runtime::RecompiledFunction orochiPadReadOriginal{};

void readOrochiPad(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const auto address=getRegU32(ctx,6);
    auto *data=getMemPtr(ram,address);
    // The SDK writes a complete 32-byte packet. Reject spans that cross a
    // memory mapping or wrap around instead of letting the generic stub write.
    if(!data || address>0xffffffe0u || getMemPtr(ram,address+31)!=data+31) {
        setReturnS32(ctx,0);
        ctx->pc=getRegU32(ctx,31);
        return;
    }
    orochiPadReadOriginal(ram,ctx,runtime);
    // USA libpad returns the byte count, checked by the caller at 0x10416c.
    // Only a successful real read is converted; disconnected ports stay 0.
    if(getRegU32(ctx,2)==1u) setReturnS32(ctx,32);
}
}

bool installOrochiPadFunctions(PS2Runtime &runtime) {
    const auto original=runtime.lookupFunction(orochiPadReadPc);
    if(!original || original==readOrochiPad) return false;
    orochiPadReadOriginal=original;
    return runtime.replaceFunction(orochiPadReadPc,readOrochiPad);
}

int runPadRegressionTests() {
    auto ownedRuntime=std::make_unique<PS2Runtime>();
    auto &runtime=*ownedRuntime;
    if(!runtime.memory().initialize()) return 2;
    auto *ram=runtime.memory().getRDRAM();
    R5900Context context{};
    auto reg=[&](unsigned index,uint32_t value) {context.r[index]=_mm_set_epi64x(0,value);};
    ps2_stubs::scePadInit(ram,&context,&runtime);
    reg(4,0); reg(5,0); reg(6,0x4000);
    ps2_stubs::scePadPortOpen(ram,&context,&runtime);
    if(getRegU32(&context,2)!=1) return 3;
    ps2_stubs::setPadOverrideState(0xfff7u,11,22,33,44);
    reg(6,0x5000); reg(31,0x104168);
    std::memset(ram+0x4fff,0xaa,34);
    runtime.lookupFunction(orochiPadReadPc)(ram,&context,&runtime);
    uint8_t baseline[32]; std::memcpy(baseline,ram+0x5000,32);
    const bool genericSuccess=getRegU32(&context,2)==1u;
    if(!installOrochiPadFunctions(runtime)) return 4;
    context.pc=orochiPadReadPc;
    runtime.lookupFunction(orochiPadReadPc)(ram,&context,&runtime);
    const bool packet=genericSuccess && getRegU32(&context,2)==32u &&
        !std::memcmp(baseline,ram+0x5000,32) && ram[0x4fff]==0xaa && ram[0x5020]==0xaa &&
        ram[0x5002]==0xf7 && ram[0x5003]==0xff && ram[0x5004]==33 && ram[0x5006]==11;
    std::cout << "[Pad regression] successful read returns 32 and preserves packet/guards: "
        << (packet?"PASS":"FAIL") << '\n';
    // Execute the actual recompiled caller's return-value check, without an ISO.
    context.pc=0x104158; reg(16,0x6000); reg(19,0); reg(29,0x9000);
    runtime.lookupFunction(0x104138)(ram,&context,&runtime);
    const bool caller=context.pc==0x104188u;
    std::cout << "[Pad regression] original game caller accepts the packet: " << (caller?"PASS":"FAIL") << '\n';
    reg(4,1); reg(5,0); reg(6,0x5000); reg(31,0x104168);
    std::memset(ram+0x5000,0xaa,32);
    runtime.lookupFunction(orochiPadReadPc)(ram,&context,&runtime);
    const bool closed=getRegU32(&context,2)==0u &&
        std::all_of(ram+0x5000,ram+0x5020,[](uint8_t byte){return byte==0xaa;});
    std::cout << "[Pad regression] unopened port stays a failed read without writes: " << (closed?"PASS":"FAIL") << '\n';
    reg(4,0); reg(6,PS2_RAM_SIZE-16);
    std::memset(ram+PS2_RAM_SIZE-16,0xaa,16);
    runtime.lookupFunction(orochiPadReadPc)(ram,&context,&runtime);
    const bool boundary=getRegU32(&context,2)==0u &&
        std::all_of(ram+PS2_RAM_SIZE-16,ram+PS2_RAM_SIZE,[](uint8_t byte){return byte==0xaa;});
    std::cout << "[Pad regression] packet crossing RAM boundary fails without writes: " << (boundary?"PASS":"FAIL") << '\n';
    ps2_stubs::clearPadOverrideState();
    return packet && caller && closed && boundary?0:1;
}
