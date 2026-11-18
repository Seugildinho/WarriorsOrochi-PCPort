#include "graphics_trace.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <cstring>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

namespace {
PS2Runtime::RecompiledFunction originalSetDisplay = nullptr, originalPutDisplay = nullptr;
PS2Runtime::RecompiledFunction originalVSync = nullptr;
PS2Runtime::RecompiledFunction originalImageDecode = nullptr, originalImageIrq = nullptr;
unsigned setCount = 0, putCount = 0;
unsigned vsyncCount = 0;
unsigned imageCount = 0, imageIrqCount = 0;
constexpr std::array<uint32_t,26> inputConsumerPcs={0x2a64f0,0x2e8490,0x2e9c50,0x2ea120,0x2ea730,0x2eb1e0,
    0x195a60,0x1f0d80,0x1f1dc0,0x229340,0x25a930,0x29e500,0x2ad030,0x2b9c30,0x2bb340,0x2bbcf0,0x2bcb70,
    0x19b820,0x1e3180,0x1f5080,0x2125f0,0x2a1f10,0x2a2a50,0x2a3690,0x2a3b30,0x2b7a90};
std::array<PS2Runtime::RecompiledFunction,inputConsumerPcs.size()> inputConsumers{};
std::array<unsigned,inputConsumerPcs.size()> inputConsumerLogs{};
PS2Runtime::RecompiledFunction originalInputEdges{};
unsigned inputEdgeLogs{};
template<unsigned Index> void traceInputConsumer(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const auto pressed=runtime->memory().read16(0x3fb980+0x4c);
    if(inputConsumerLogs[Index]==0 || (pressed && inputConsumerLogs[Index]<32)) {
        ++inputConsumerLogs[Index];
        std::cout << "[Orochi:input-consumer] pc=0x" << std::hex << inputConsumerPcs[Index]
            << " resume=0x" << ctx->pc
            << " ra=0x" << getRegU32(ctx,31) << " held=0x" << runtime->memory().read16(0x3fb980+0x48)
            << " pressed=0x" << pressed << std::dec << '\n';
    }
    inputConsumers[Index](ram,ctx,runtime);
}
template<size_t... Index> auto inputConsumerTraces(std::index_sequence<Index...>) {
    return std::array<PS2Runtime::RecompiledFunction,sizeof...(Index)>{traceInputConsumer<Index>...};
}
void traceInputEdges(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const auto address=getRegU32(ctx,4),returnPc=getRegU32(ctx,31);
    originalInputEdges(ram,ctx,runtime);
    if(ctx->pc==returnPc && address<PS2_RAM_SIZE-0x50 && inputEdgeLogs<24) {
        const auto pressed=runtime->memory().read16(address+0x4c);
        if(pressed) {
            ++inputEdgeLogs;
            std::cout << "[Orochi:input-edge] controller=0x" << std::hex << address
                << " held=0x" << runtime->memory().read16(address+0x48)
                << " pressed=0x" << pressed << std::dec << '\n';
        }
    }
}
uint32_t readWord(uint8_t *ram,uint32_t address) {
    uint32_t value{}; const auto *data=getConstMemPtr(ram,address);
    if(data) std::memcpy(&value,data,4);
    return value;
}
void traceImageDecode(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const uint32_t asset=getRegU32(ctx,4);
    if(imageCount++ < 4) {
        std::cout << "[Orochi:image-decode] asset=0x" << std::hex << asset
            << " ra=0x" << getRegU32(ctx,31) << " selection=" << std::dec << getRegU32(ctx,5);
        for(unsigned i=0;i<8;i++) std::cout << " w" << i << "=0x" << std::hex << readWord(ram,asset+i*4);
        std::cout << std::dec << '\n';
        const auto dataAddress=readWord(ram,asset+8)&0x1fffffffu;
        if(imageCount==1 && dataAddress<PS2_RAM_SIZE) {
            const auto path=PS2Runtime::getIoPaths().elfDirectory / "image-decode-input.bin";
            std::ofstream out(path,std::ios::binary);
            out.write(reinterpret_cast<const char *>(ram+dataAddress),std::min(512u*1024u,PS2_RAM_SIZE-dataAddress));
        }
    }
    originalImageDecode(ram,ctx,runtime);
}
void traceImageIrq(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    if(imageIrqCount++ < 8) std::cout << "[Orochi:image-irq] cause=" << getRegU32(ctx,4) << '\n';
    originalImageIrq(ram,ctx,runtime);
}
void traceVSync(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    if (vsyncCount++ < 8) std::cout << "[Orochi:vsync] cause=" << getRegU32(ctx,4)
        << " gp=0x" << std::hex << getRegU32(ctx,28) << std::dec << '\n';
    originalVSync(ram,ctx,runtime);
}
void printEnvironment(uint8_t *ram, uint32_t address) {
    const auto *data = getConstMemPtr(ram, address);
    if (!data) return;
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t value{}; std::memcpy(&value,data+i*8,8);
        std::cout << " w" << i << "=0x" << std::hex << value << std::dec;
    }
}
void traceSetDisplay(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    const auto address=getRegU32(ctx,4),caller=getRegU32(ctx,31);
    const bool trace=setCount++<16;
    if (trace) std::cout << "[Orochi:display-set] env=0x" << std::hex << address << " ra=0x" << caller
        << std::dec << " psm=" << getRegU32(ctx,5) << " width=" << getRegU32(ctx,6)
        << " height=" << getRegU32(ctx,7) << '\n';
    originalSetDisplay(ram,ctx,runtime);
    if (trace) { std::cout << "[Orochi:display-set-result]"; printEnvironment(ram,address); std::cout << '\n'; }
}
void tracePutDisplay(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime) {
    if (putCount++<24) {
        std::cout << "[Orochi:display-put] env=0x" << std::hex << getRegU32(ctx,4)
                  << " ra=0x" << getRegU32(ctx,31) << std::dec;
        printEnvironment(ram,getRegU32(ctx,4)); std::cout << '\n';
    }
    originalPutDisplay(ram,ctx,runtime);
}
}
bool installOrochiGraphicsTrace(PS2Runtime &runtime) {
    originalSetDisplay=runtime.lookupFunction(0x134820);
    originalPutDisplay=runtime.lookupFunction(0x134b98);
    originalVSync=runtime.lookupFunction(0x101550);
    originalImageDecode=runtime.lookupFunction(0x152560);
    originalImageIrq=runtime.lookupFunction(0x152870);
    if(std::getenv("OROCHI_TEST_START") || std::getenv("OROCHI_TEST_INPUTS")) {
        const auto traces=inputConsumerTraces(std::make_index_sequence<inputConsumerPcs.size()>{});
        for(unsigned i=0;i<inputConsumerPcs.size();i++) {
            inputConsumers[i]=runtime.lookupFunction(inputConsumerPcs[i]); inputConsumerLogs[i]=0;
            if(!inputConsumers[i] || !runtime.replaceFunction(inputConsumerPcs[i],traces[i])) return false;
        }
        // Recompiled functions can yield and re-enter at named continuation
        // PCs. Observe those aliases too, preserving their original ctx->pc.
        for(uint32_t pc=0x100008;pc<0x3068e8;pc+=4) {
            if(!runtime.hasFunction(pc)) continue;
            const auto function=runtime.lookupFunction(pc);
            if(!function) continue;
            for(unsigned i=0;i<inputConsumers.size();i++)
                if(function==inputConsumers[i] && !runtime.replaceFunction(pc,traces[i])) return false;
        }
        inputEdgeLogs=0; originalInputEdges=runtime.lookupFunction(0x104990);
        if(!originalInputEdges || !runtime.replaceFunction(0x104990,traceInputEdges) ||
           !runtime.replaceFunction(0x1049c4,traceInputEdges)) return false;
    }
    return originalSetDisplay && originalPutDisplay && originalVSync && originalImageDecode && originalImageIrq &&
        runtime.replaceFunction(0x134820,traceSetDisplay) && runtime.replaceFunction(0x134b98,tracePutDisplay) &&
        runtime.replaceFunction(0x101550,traceVSync) && runtime.replaceFunction(0x152560,traceImageDecode) &&
        runtime.replaceFunction(0x152870,traceImageIrq);
}
