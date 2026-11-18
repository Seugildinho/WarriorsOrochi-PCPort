#include "../vendor/PS2Recomp/ps2xIOP/src/emulator/core/iop_memory.h"
#include "../vendor/PS2Recomp/ps2xIOP/src/emulator/core/iop_cpu.h"
#include "../vendor/PS2Recomp/ps2xIOP/src/emulator/imports/iop_sysmem.h"
#include "../vendor/PS2Recomp/ps2xIOP/src/emulator/imports/iop_heaplib.h"
#include "../vendor/PS2Recomp/ps2xIOP/tests/iop_compat_test_support.h"
#include <iostream>

int runIopMemoryRegressionTests() {
    using ps2x::iop::detail::IopMemory;
    IopMemory memory;
    const auto base=IopMemory::HeapBase, limit=IopMemory::HeapLimit, capacity=limit-base;
    bool reused=true;
    // The original music driver requests 0x52000 bytes for its 161-sector ring
    // buffer. Repeated scene changes must not consume already freed space.
    for(unsigned round=0;round<100;round++) {
        const auto buffer=memory.allocate(0x52000);
        reused &= buffer==base;
        if(!buffer || !memory.freeAllocation(buffer)) {reused=false;break;}
    }
    const auto fixed=memory.allocate(0x10000,16,base+0x20000);
    const bool fragmented=fixed==base+0x20000 && memory.maxFreeMemory()==capacity-0x30000 &&
        memory.totalFreeMemory()==capacity-0x10000;
    const auto hole=memory.allocate(0x18000);
    const bool filled=hole==base && memory.maxFreeMemory()==capacity-0x30000 &&
        memory.totalFreeMemory()==capacity-0x28000;
    const bool guarded=memory.allocate(0x20000,16,base+0x18000)==0 &&
        memory.allocate(0xffffffff)==0 && memory.allocate(16,3,0xfffffff0)==0 &&
        memory.allocate(16,24)==0;
    const bool released=memory.freeAllocation(fixed) && memory.freeAllocation(hole) &&
        !memory.freeAllocation(hole) && memory.maxFreeMemory()==capacity && memory.totalFreeMemory()==capacity;
    const auto aligned=memory.allocate(17,64);
    const auto aligned2=memory.allocate(17,64);
    const bool alignment=aligned==base && aligned2==base+64 && memory.ownsRamRange(aligned2,32) &&
        !memory.ownsRamRange(aligned2,33);
    std::cout << "[IOP memory regression] 100 music-buffer allocation/free cycles reuse space: " << (reused?"PASS":"FAIL") << '\n';
    std::cout << "[IOP memory regression] fragmented heap reports largest and total free bytes: " << (fragmented && filled && released?"PASS":"FAIL") << '\n';
    std::cout << "[IOP memory regression] fixed blocks, overflow, overlap, alignment and ownership: " << (guarded && alignment?"PASS":"FAIL") << '\n';
    // Exercise the public sysmem import, not merely the allocator helper.
    // FIRST and LAST must occupy opposite ends of real IOP RAM, in 256-byte units.
    using namespace ps2x::iop::detail;
    IopMemory sysMemory;
    iop_test::Host host;
    IopSysmem sysmem(host,sysMemory);
    IopCpuState cpu{};
    auto request=[&](uint32_t mode,uint32_t bytes,uint32_t address=0) {
        cpu.gpr[4]=mode;cpu.gpr[5]=bytes;cpu.gpr[6]=address;
        if(!sysmem.dispatchImport(4,cpu)) return uint32_t(0);
        return cpu.gpr[2];
    };
    const auto first=request(0,0x101),last=request(1,0x101);
    const bool oppositeEnds=first==base && last==limit-0x200 &&
        sysMemory.allocationContaining(first)->size==0x200 &&
        sysMemory.allocationContaining(last)->size==0x200;
    const auto fixedAt=base+0x50000;
    const auto fixedBlock=request(2,0x101,fixedAt);
    const auto spaceBeforeInvalid=sysMemory.totalFreeMemory();
    const bool invalidRejected=request(3,16)==0 && request(0,0)==0 &&
        request(1,0xffffffff)==0 && request(2,256,fixedAt+1)==0 &&
        request(2,0x200,fixedAt)==0 && sysMemory.totalFreeMemory()==spaceBeforeInvalid;
    const bool fixedUnits=fixedBlock==fixedAt && sysMemory.allocationContaining(fixedAt)->size==0x200;
    sysMemory.write32(first,0x12345678);sysMemory.write32(last,0x87654321);
    // The highest fitting gap is below an occupied high block; reusing a freed
    // high gap must not damage either live end or an unrelated fixed module.
    const auto high=request(1,0x1000),lowerHigh=request(1,0x1000);
    const bool freeHigh=sysMemory.freeAllocation(high);
    const auto highAgain=request(1,0x1000);
    const bool lastReuse=freeHigh && high==limit-0x1200 && lowerHigh==limit-0x2200 &&
        highAgain==high && sysMemory.read32(first)==0x12345678 && sysMemory.read32(last)==0x87654321;
    std::cout << "[IOP memory regression] sysmem FIRST/LAST use opposite ends and 256-byte blocks: "
        << (oppositeEnds?"PASS":"FAIL") << '\n';
    std::cout << "[IOP memory regression] sysmem ADDRESS, invalid modes/sizes and high-gap reuse: "
        << (fixedUnits && invalidRejected && lastReuse?"PASS":"FAIL") << '\n';
    // Each movie creates a heap with one 80-sector stream chunk and deletes the
    // heap without FreeHeapMemory. The third cycle used to starve the 0x52000
    // music buffer requested when a stage loads after the story scene.
    IopMemory heapMemory;
    IopHeaplib heaplib(heapMemory);
    auto heapCall=[&](uint16_t ordinal,uint32_t a0,uint32_t a1=0) {
        cpu.gpr[4]=a0;cpu.gpr[5]=a1;
        if(!heaplib.dispatchImport(ordinal,cpu)) return uint32_t(0xffffffffu);
        return cpu.gpr[2];
    };
    bool heapReleased=true;
    for(unsigned movie=0;movie<3;movie++) {
        const auto heap=heapCall(4,0x28000);
        const auto chunk=heapCall(6,heap,0x28000);
        heapReleased &= heap!=0 && chunk!=0 && heapCall(5,heap)==0 &&
            heapMemory.totalFreeMemory()==capacity;
    }
    const auto music=heapMemory.allocate(0x52000);
    const auto keep=heapMemory.allocate(0x100);
    const auto heap=heapCall(4,0x1000);
    const auto chunk=heapCall(6,heap,0x200);
    // A chunk already returned by FreeHeapMemory must not be freed again by
    // DeleteHeap, even after its address is reused by an unrelated block.
    const bool chunkFreed=heapCall(7,heap,chunk)==0;
    const auto reuse=heapMemory.allocate(0x200);
    const bool noDoubleFree=chunkFreed && reuse==chunk && heapCall(5,heap)==0 &&
        heapMemory.allocationContaining(reuse) && heapMemory.allocationContaining(keep);
    std::cout << "[IOP memory regression] heaplib DeleteHeap releases undeleted chunks; music buffer fits after 3 movies: "
        << (heapReleased && music!=0 && noDoubleFree?"PASS":"FAIL") << '\n';
    return reused && fragmented && filled && guarded && released && alignment &&
        oppositeEnds && fixedUnits && invalidRejected && lastReuse &&
        heapReleased && music!=0 && noDoubleFree?0:1;
}
