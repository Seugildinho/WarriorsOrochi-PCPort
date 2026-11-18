#include "native_sif.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.h"
#include <array>
#include <iostream>
#include <memory>

namespace {
constexpr uint32_t commandData=0x4315d8,softwareRegs=0x431780;
// sceSifFreeIopHeap (wrapper) and sceSifFreeSysMemory in the USA ELF.
constexpr uint32_t freeIopHeapPc=0x13f100,freeSysMemoryPc=0x13f088;
PS2Runtime::RecompiledFunction originalInitCmd{},originalInitRpc{},originalSetSregHandler{};
// sceSifAllocIopHeap (0x13ef98) is a native stub that allocates from the
// emulated IOP heap, but the matching frees stayed as the original SIF RPC
// client, which never returns memory to that allocator. Each movie leaked its
// 0x28010-byte stream buffer until the 0x52000 stage music buffer no longer fit.
void freeIopHeap(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    ctx->pc=getRegU32(ctx,31);
    ps2_stubs::sceSifFreeIopHeap(ram,ctx,runtime);
}
void freeSysMemory(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    ctx->pc=getRegU32(ctx,31);
    ps2_stubs::sceSifFreeSysMemory(ram,ctx,runtime);
}
unsigned packetLogs{};
std::array<uint32_t,32> lastRegisterValues{};
std::array<bool,32> seenRegisters{};
void observeSetSreg(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const auto packet=getRegU32(ctx,4);
    const auto index=runtime->memory().read32(packet+16),value=runtime->memory().read32(packet+20);
    originalSetSregHandler(ram,ctx,runtime);
    // Keep the initial packets and later state changes visible. Periodic zero
    // reports otherwise consume the log budget before a music request occurs.
    if(index<32 && (packetLogs<16 || !seenRegisters[index] || lastRegisterValues[index]!=value)) {
        ++packetLogs;
        std::cout << "[Orochi:sif-sreg] index=" << index << " received=0x" << std::hex << value
            << " guest=0x" << runtime->memory().read32(softwareRegs+index*4) << std::dec << '\n';
    }
    if(index<32) {seenRegisters[index]=true;lastRegisterValues[index]=value;}
}
void initializeCommands(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    originalInitCmd(ram,ctx,runtime);
    ensureOrochiSifCommandHandlers(*runtime);
}
void initializeRpc(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    originalInitRpc(ram,ctx,runtime);
    ensureOrochiSifCommandHandlers(*runtime);
}
unsigned testMainCalls{};
bool testReaderPassed{};
void sifTestMain(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    if(runtime->memory().read32(softwareRegs+24*4)==1) {
        // Exercise the game's actual sound-status reader and its GetSreg call.
        R5900Context reader{};reader.pc=0x118e28;
        reader.r[28]=_mm_set_epi64x(0,0x360170);
        reader.r[29]=_mm_set_epi64x(0,0x9000);
        reader.r[31]=_mm_set_epi64x(0,0x100104);
        runtime->lookupFunction(reader.pc)(ram,&reader,runtime);
        testReaderPassed=getRegU32(&reader,2)==1 && reader.pc==0x100104;
        ctx->pc=0;runtime->requestStop();return;
    }
    if(++testMainCalls>1000) {ctx->pc=0;runtime->requestStop();return;}
    ctx->pc=0x100100;
}
}

void ensureOrochiSifCommandHandlers(PS2Runtime &runtime) {
    auto &memory=runtime.memory();
    // Exact command-data layout from original sceSifInitCmd at 0x139ac0.
    // Native SDK initialization skips this guest structure, while the game's
    // recompiled GetSreg still reads it. Repeated init must preserve live data.
    if(memory.read32(0x305388)==0) {
        memory.write32(0x305388,1);
        for(uint32_t address=0x431600;address<0x431800;address+=4) memory.write32(address,0);
        memory.write32(commandData,0x20431500);
        memory.write32(commandData+4,0x20431580);
        memory.write32(commandData+8,0);
        memory.write32(commandData+12,0x431600);
        memory.write32(commandData+16,32);
        memory.write32(commandData+20,0);
        memory.write32(commandData+24,0);
        memory.write32(commandData+28,softwareRegs);
        memory.write32(0x431600,0x139a68);
        memory.write32(0x431604,commandData);
        memory.write32(0x43160c,0x139a48);
        memory.write32(0x431610,commandData);
    }
    // Route genuine IOP packets to the original SDK system-command handlers.
    for(const auto item:std::array<std::array<uint32_t,2>,2>{{
            {0x80000000,0x139a68},{0x80000001,0x139a48}}}) {
        R5900Context registration{};
        registration.r[4]=_mm_set_epi64x(0,item[0]);
        registration.r[5]=_mm_set_epi64x(0,item[1]);
        registration.r[6]=_mm_set_epi64x(0,commandData);
        ps2_stubs::sceSifAddCmdHandler(memory.getRDRAM(),&registration,&runtime);
    }
}

bool installOrochiSifFunctions(PS2Runtime &runtime) {
    originalInitCmd=runtime.lookupFunction(0x139ac0);
    originalInitRpc=runtime.lookupFunction(0x13a220);
    originalSetSregHandler=runtime.lookupFunction(0x139a48);
    packetLogs=0;
    seenRegisters.fill(false);lastRegisterValues.fill(0);
    const auto originalFreeIopHeap=runtime.lookupFunction(freeIopHeapPc);
    const auto originalFreeSysMemory=runtime.lookupFunction(freeSysMemoryPc);
    if(!originalInitCmd || !originalInitRpc || !originalSetSregHandler ||
       originalInitCmd==initializeCommands || originalInitRpc==initializeRpc ||
       originalSetSregHandler==observeSetSreg || !originalFreeIopHeap || !originalFreeSysMemory ||
       originalFreeIopHeap==freeIopHeap || originalFreeSysMemory==freeSysMemory) return false;
    return runtime.replaceFunction(0x139ac0,initializeCommands) &&
        runtime.replaceFunction(0x13a220,initializeRpc) &&
        runtime.replaceFunction(0x139a48,observeSetSreg) &&
        runtime.replaceFunction(freeIopHeapPc,freeIopHeap) &&
        runtime.replaceFunction(freeSysMemoryPc,freeSysMemory);
}

int runSifRegressionTests() {
    auto runtime=std::make_unique<PS2Runtime>();
    if(!runtime->memory().initialize() || !installOrochiSifFunctions(*runtime)) return 2;
    auto *ram=runtime->memory().getRDRAM();
    ps2_stubs::resetSifState();
    runtime->configureGuestHeap(0x1000000,PS2_RAM_SIZE);
    R5900Context main{};main.pc=0x100100;
    main.r[28]=_mm_set_epi64x(0,0x360170);main.r[29]=_mm_set_epi64x(0,0x8000);
    runtime->registerFunction(main.pc,sifTestMain);
    runtime->eeScheduler().reset(ram,main);
    const uint32_t packet[6]={24,0,0x80000001,0,24,1};
    const bool absent=!ps2_stubs::dispatchSifCommand(ram,runtime.get(),0x80000001,packet,sizeof(packet));
    ensureOrochiSifCommandHandlers(*runtime);
    const bool queued=ps2_stubs::dispatchSifCommand(ram,runtime.get(),0x80000001,packet,sizeof(packet));
    testMainCalls=0;testReaderPassed=false;
    runtime->eeScheduler().run();
    const bool delivered=runtime->memory().read32(softwareRegs+24*4)==1;
    ensureOrochiSifCommandHandlers(*runtime);
    const bool preserved=runtime->memory().read32(softwareRegs+24*4)==1;
    std::cout << "[SIF regression] native transport delivers packet to original handler and game reader: "
        << (absent && queued && delivered && testReaderPassed?"PASS":"FAIL") << '\n';
    std::cout << "[SIF regression] repeated initialization preserves received state: "
        << (preserved?"PASS":"FAIL") << '\n';
    // Three movie stream buffers allocated by the stub and released through the
    // game's own free entry points; a second free of the same block must fail.
    auto freeThrough=[&](uint32_t pc,uint32_t address) {
        R5900Context call{};
        call.r[4]=_mm_set_epi64x(0,address);call.r[31]=_mm_set_epi64x(0,0x114e78);
        runtime->lookupFunction(pc)(ram,&call,runtime.get());
        return call.pc==0x114e78?static_cast<int32_t>(getRegU32(&call,2)):-2;
    };
    bool heapReleased=true;
    for(unsigned movie=0;movie<3;movie++) {
        const auto stream=runtime->allocateIopMemory(0x28010,64);
        heapReleased &= stream!=0 && freeThrough(freeIopHeapPc,stream)==0 &&
            freeThrough(freeIopHeapPc,stream)==-1;
    }
    const auto sysBlock=runtime->allocateIopMemory(0x400,64);
    const bool sysReleased=sysBlock!=0 && freeThrough(freeSysMemoryPc,sysBlock)==0 &&
        !runtime->freeIopMemory(sysBlock);
    std::cout << "[SIF regression] game IOP-heap frees release stub allocations (3 movie buffers): "
        << (heapReleased && sysReleased?"PASS":"FAIL") << '\n';
    return absent && queued && delivered && testReaderPassed && preserved &&
        heapReleased && sysReleased?0:1;
}
