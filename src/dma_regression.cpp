#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

// Distinct payloads at identical RAM/SPR offsets catch accidental CPU-style
// decoding of DMAC addresses. These tests require no game files or window.
int runDmaRegressionTests() {
    auto ownedMemory = std::make_unique<PS2Memory>();
    auto &memory = *ownedMemory;
    if (!memory.initialize()) return 2;
    std::vector<uint8_t> packet;
    memory.setGifPacketCallback([&](const uint8_t *data, uint32_t size) { packet.assign(data, data + size); });
    std::memset(memory.getRDRAM() + 0x3000, 0x11, 16);
    std::memset(memory.getScratchpad() + 0x3000, 0x77, 16);
    std::memset(memory.getScratchpad() + 0x2010, 0x55, 16);
    int failures = 0;
    auto verify = [&](const char *name, uint8_t expected) {
        const bool ok = packet.size() == 16 && std::all_of(packet.begin(), packet.end(),
            [expected](uint8_t byte) { return byte == expected; });
        std::cout << "[DMA regression] " << name << ": " << (ok ? "PASS" : "FAIL") << '\n';
        failures += !ok;
    };
    auto submit = [&](uint32_t address, bool chain) {
        packet.clear();
        memory.writeIORegister(0x1000a020, chain ? 0 : 1);
        memory.writeIORegister(chain ? 0x1000a030 : 0x1000a010, address);
        memory.writeIORegister(0x1000a000, chain ? 0x105 : 0x101);
        memory.processPendingTransfers();
    };
    submit(0x00003000, false); verify("normal DMA reads main RAM", 0x11);
    submit(0x80003000, false); verify("normal DMA SPR bit reads scratchpad", 0x77);
    // REFE, one quadword, SPR address in the high half of the tag.
    memory.write64(0x4000, 1ull | (0x80003000ull << 32));
    memory.write64(0x4008, 0);
    submit(0x4000, true); verify("REFE tag preserves ADDR.SPR", 0x77);
    // END tag and its inline payload both reside in scratchpad.
    memory.write64(0x70002000, 1ull | (7ull << 28));
    memory.write64(0x70002008, 0);
    submit(0x80002000, true); verify("chain TADR.SPR reads scratchpad", 0x55);
    const bool cpuAlias = memory.read32(0x80003000) == 0x11111111;
    std::cout << "[DMA regression] CPU KSEG0 remains main RAM: " << (cpuAlias ? "PASS" : "FAIL") << '\n';
    memory.writeIORegister(0x1000b000,0x100);
    memory.writeIORegister(0x1000b400,0x101);
    const bool ipuBusy=memory.readIORegister(0x1000b000)==0x100 &&
        memory.readIORegister(0x1000b000)==0x100 && memory.readIORegister(0x1000b400)==0x101;
    std::cout << "[DMA regression] IPU STR survives status reads until real completion: " << (ipuBusy ? "PASS" : "FAIL") << '\n';
    return failures + !cpuAlias + !ipuBusy ? 1 : 0;
}

namespace {
// Addresses must lie inside this runner's generated dispatch table.
constexpr uint32_t callbackPc = 0x00100100, waitPc = 0x00100104;
unsigned callbackHits{}, callbackFailures{};
unsigned presentationFailures{};
void regressionCallback(uint8_t *, R5900Context *ctx, PS2Runtime *runtime) {
    ++callbackHits;
    callbackFailures += getRegU32(ctx,4) != 2u;
    callbackFailures += getRegU32(ctx,28) != 0x12345678u;
    // Interrupt handlers run on the kernel-area stacks, outside game memory.
    callbackFailures += getRegU32(ctx,29) < 0x00020000u || getRegU32(ctx,29) >= 0x00080000u;
    std::vector<uint8_t> before,after; uint32_t width{},height{};
    const auto sequence=runtime->gs().hostPresentationSequence();
    const bool ready=runtime->gs().copyLatchedHostPresentationFrame(before,width,height);
    const auto expected=callbackHits&1u ? 0xffu : 0x00u;
    presentationFailures += !ready || width!=64 || height!=64 || before.empty() || before[0]!=expected;
    // Begin the next frame: the window must still see the completed one.
    runtime->gs().WriteVram(0,0,1,0,0,callbackHits&1u ? 0x8000ff00u : 0x800000ffu);
    runtime->gs().copyLatchedHostPresentationFrame(after,width,height);
    presentationFailures += before!=after || sequence!=runtime->gs().hostPresentationSequence();
    ctx->pc = 0;
}
void regressionWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime) {
    if (callbackHits >= 3) { ctx->pc=0; runtime->requestStop(); return; }
    ctx->pc=waitPc;
    runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick());
}
}

// Three successive events distinguish a constant INTC cause from frame ticks.
// This exercises the real scheduler without a game disc or a graphical window.
int runVSyncRegressionTests() {
    callbackHits=callbackFailures=presentationFailures=0;
    auto ownedRuntime = std::make_unique<PS2Runtime>();
    auto &runtime = *ownedRuntime;
    if (!runtime.memory().initialize()) return 2;
    auto &registers=runtime.memory().gs();
    registers.pmode=1; registers.smode2=0;
    registers.dispfb1=1ull<<9; registers.display1=(63ull<<32)|(63ull<<44);
    runtime.gs().init(runtime.memory().getGSVRAM(),PS2_GS_VRAM_SIZE,&registers);
    runtime.gs().WriteVram(0,0,1,0,0,0x800000ffu);
    runtime.configureGuestHeap(0x01f00000u,0x01f00000u);
    if (!runtime.registerFunction(callbackPc,regressionCallback) ||
        !runtime.registerFunction(waitPc,regressionWait)) return 3;
    R5900Context context{}; context.pc=waitPc;
    runtime.eeScheduler().reset(runtime.memory().getRDRAM(),context);
    runtime.eeScheduler().setGsVSyncCallback(callbackPc,0x12345678u,0);
    runtime.eeScheduler().run();
    const bool passed=callbackHits==3 && callbackFailures==0;
    std::cout << "[VSync regression] three callbacks preserve cause, GP and isolated stack: "
              << (passed ? "PASS" : "FAIL") << " hits=" << callbackHits
              << " failures=" << callbackFailures << '\n';
    std::cout << "[VSync regression] scanout freezes before callback and survives next-frame writes: "
        << (presentationFailures==0 ? "PASS" : "FAIL") << " failures=" << presentationFailures << '\n';
    return passed && presentationFailures==0 ? 0 : 1;
}

int runGifRegressionTests() {
    std::vector<uint8_t> fullVram(PS2_GS_VRAM_SIZE),splitVram(PS2_GS_VRAM_SIZE),packet;
    GSRegisters fullRegisters{},splitRegisters{};
    fullRegisters.dispfb1=splitRegisters.dispfb1=0x1400;
    auto full=std::make_unique<GS>(), split=std::make_unique<GS>();
    full->init(fullVram.data(),fullVram.size(),&fullRegisters);
    split->init(splitVram.data(),splitVram.size(),&splitRegisters);
    auto append=[&](uint64_t value) { const auto start=packet.size(); packet.resize(start+8); std::memcpy(packet.data()+start,&value,8); };
    append(4ull|(1ull<<60)); append(0xe);
    append((64ull<<32)|(1ull<<48)); append(0x50);
    append(0); append(0x51);
    append(4ull|(4ull<<32)); append(0x52);
    append(0); append(0x53);
    append(4ull|(1ull<<15)|(2ull<<58)); append(0);
    for(unsigned i=0;i<8;i++) append(0x8059595980595959ull);
    full->processGIFPacket(packet.data(),packet.size());
    split->processGIFPacket(packet.data(),96); // setup plus IMAGE tag, no pixels
    split->processGIFPacket(packet.data()+96,16);
    split->processGIFPacket(packet.data()+112,48);
    const bool pixels=std::any_of(fullVram.begin(),fullVram.end(),[](uint8_t b){return b!=0;});
    const bool passed=pixels && fullVram==splitVram && splitRegisters.dispfb1==0x1400;
    std::cout << "[GIF regression] fragmented IMAGE matches complete upload and preserves display registers: "
        << (passed ? "PASS" : "FAIL") << '\n';
    // Interleave a second path while PATH3 is waiting for IMAGE pixels.
    std::vector<std::vector<uint8_t>> received;
    GifArbiter arbiter([&](const uint8_t *data,uint32_t bytes) { received.emplace_back(data,data+bytes); });
    arbiter.submit(GifPathId::Path3,packet.data(),96); arbiter.drain();
    const bool held=received.empty() && !arbiter.empty();
    const uint64_t nopTag[2]={1ull<<15,0};
    arbiter.submit(GifPathId::Path2,reinterpret_cast<const uint8_t *>(nopTag),16); arbiter.drain();
    arbiter.submit(GifPathId::Path3,packet.data()+96,64); arbiter.drain();
    const bool isolated=held && received.size()==2 && received[0].size()==16 && received[1]==packet && arbiter.empty();
    std::cout << "[GIF regression] interleaved paths retain their own incomplete image packet: " << (isolated ? "PASS" : "FAIL") << '\n';
    auto memory=std::make_unique<PS2Memory>();
    if(!memory->initialize()) return 2;
    auto vifGs=std::make_unique<GS>();
    vifGs->init(memory->getGSVRAM(),PS2_GS_VRAM_SIZE,&memory->gs());
    memory->setGifPacketCallback([&](const uint8_t *data,uint32_t bytes) { vifGs->processGIFPacket(data,bytes); });
    // The IMAGE tag is in one DIRECTHL; its pixels arrive in a second command.
    // VIF must parse the second command, without fabricating another GIF tag.
    std::vector<uint8_t> setup(16+96),image(16+64);
    const uint32_t setupCommand=0x51000006,imageCommand=0x51000004;
    std::memcpy(setup.data()+12,&setupCommand,4); std::memcpy(setup.data()+16,packet.data(),96);
    std::memcpy(image.data()+12,&imageCommand,4); std::memcpy(image.data()+16,packet.data()+96,64);
    memory->processVIF1Data(setup.data(),setup.size());
    memory->processVIF1Data(image.data(),image.size());
    const bool vifMatches=std::equal(fullVram.begin(),fullVram.end(),memory->getGSVRAM());
    std::cout << "[GIF regression] separate VIF DIRECTHL commands preserve IMAGE payload: " << (vifMatches ? "PASS" : "FAIL") << '\n';
    vifGs->reset(); std::memset(memory->getGSVRAM(),0,PS2_GS_VRAM_SIZE);
    memory->writeIORegister(0x10003c10u,1u);
    std::vector<uint8_t> whole(16+packet.size());
    const uint32_t wholeCommand=0x5100000a;
    std::memcpy(whole.data()+12,&wholeCommand,4); std::memcpy(whole.data()+16,packet.data(),packet.size());
    memory->processVIF1Data(whole.data(),16+64);
    memory->processVIF1Data(whole.data()+16+64,packet.size()-64);
    const bool directContinues=std::equal(fullVram.begin(),fullVram.end(),memory->getGSVRAM());
    std::cout << "[GIF regression] a truncated VIF DIRECTHL resumes its own payload: " << (directContinues ? "PASS" : "FAIL") << '\n';
    return passed && isolated && vifMatches && directContinues ? 0 : 1;
}

int runDisplayRegressionTests() {
    std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE);
    GSRegisters registers{};
    registers.pmode=1; registers.smode2=3;
    registers.dispfb1=1ull<<9; registers.display1=(63ull<<32)|(63ull<<44);
    auto gs=std::make_unique<GS>(); gs->init(vram.data(),vram.size(),&registers);
    const uint32_t colors[4]={0x800000ffu,0x8000ff00u,0x80ff0000u,0x80ffff00u};
    for(unsigned y=0;y<4;y++) gs->WriteVram(0,0,1,0,y,colors[y]);
    auto verify=[&](uint64_t mode,const unsigned (&rows)[4]) {
        registers.smode2=mode; gs->latchHostPresentationFrame();
        std::vector<uint8_t> rgba; uint32_t width{},height{};
        if(!gs->copyLatchedHostPresentationFrame(rgba,width,height) || width!=64 || height!=64) return false;
        for(unsigned y=0;y<4;y++) { uint32_t pixel{};std::memcpy(&pixel,rgba.data()+y*width*4,4);
            if((pixel&0xffffffu)!=(colors[rows[y]]&0xffffffu) || (pixel>>24)!=255) return false;
        }
        return true;
    };
    const bool compact=verify(3,{0,0,1,1});
    const bool progressive=verify(0,{0,1,2,3});
    const bool field=verify(1,{0,0,2,2});
    std::cout << "[Display regression] interlaced FRAME reads half-height then doubles lines: " << (compact ? "PASS" : "FAIL") << '\n';
    std::cout << "[Display regression] progressive and interlaced FIELD remain distinct: " << (progressive&&field ? "PASS" : "FAIL") << '\n';
    return compact && progressive && field ? 0 : 1;
}
