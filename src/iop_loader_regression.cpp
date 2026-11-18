#include "../vendor/PS2Recomp/ps2xIOP/src/emulator/core/iop_memory.h"
#include "../vendor/PS2Recomp/ps2xIOP/src/emulator/services/iop_module_loader.h"
#include <cstring>
#include <iostream>
#include <vector>

int runIopLoaderRegressionTests() {
    using namespace ps2x::iop::detail;
    // Synthetic MIPS ELF: one loadable segment, no game bytes or dependencies.
    std::vector<uint8_t> image(0x110);
    auto word=[&](unsigned offset,uint32_t value){std::memcpy(image.data()+offset,&value,4);};
    auto half=[&](unsigned offset,uint16_t value){std::memcpy(image.data()+offset,&value,2);};
    const uint8_t magic[]={0x7f,'E','L','F',1,1,1};
    std::memcpy(image.data(),magic,sizeof(magic));
    half(16,0xff80);half(18,8);word(20,1);word(28,52);
    half(40,52);half(42,32);half(44,1);
    word(52,1);word(56,0x100);word(68,16);word(72,0x300);word(76,7);word(80,16);
    word(0x100,0x03e00008);word(0x104,0);word(0x108,0x12345678);
    IopMemory memory;
    const auto live=memory.allocate(0x38000);
    memory.write32(live,0xaabbccdd);
    const auto first=IopModuleLoader::load(image,memory,IopMemory::HeapBase);
    const auto second=IopModuleLoader::load(image,memory,IopMemory::HeapBase);
    bool protectedImages=first && second && first.base>=live+0x38000 &&
        second.base>=first.base+first.size && memory.read32(live)==0xaabbccdd &&
        memory.read32(first.base)==0x03e00008 && memory.read32(second.base+8)==0x12345678 &&
        memory.read32(first.base+0x100)==0;
    const auto audio=memory.allocate(0x80000);
    const auto music=memory.allocate(0x52000);
    protectedImages &= audio && music && memory.read32(first.base)==0x03e00008 &&
        memory.read32(second.base+8)==0x12345678 && music+0x52000<=IopMemory::HeapLimit;
    const bool freed=first && memory.freeAllocation(first.base);
    const auto replacement=IopModuleLoader::load(image,memory,IopMemory::HeapBase);
    const bool reused=freed && replacement && replacement.base==first.base &&
        memory.read32(second.base+8)==0x12345678;
    const auto available=memory.totalFreeMemory();
    word(68,0x1000); // Truncated file after reserving its memory: must roll back.
    const auto truncated=IopModuleLoader::load(image,memory,IopMemory::HeapBase);
    const bool rollback=!truncated && memory.totalFreeMemory()==available;
    word(68,16);word(60,0xfffffff0); // Wrapped virtual extent must fail before writes.
    const auto overflow=IopModuleLoader::load(image,memory,IopMemory::HeapBase);
    const bool bounded=!overflow && memory.totalFreeMemory()==available;
    half(16,2);word(24,second.base);word(60,second.base);
    const auto collision=IopModuleLoader::load(image,memory,IopMemory::HeapBase);
    const bool fixedProtected=!collision && memory.totalFreeMemory()==available &&
        memory.read32(second.base+8)==0x12345678;
    std::cout << "[IOP loader regression] modules and large audio buffers share real RAM without overlap: "
        << (protectedImages?"PASS":"FAIL") << '\n';
    std::cout << "[IOP loader regression] released image reservation is reusable: " << (reused?"PASS":"FAIL") << '\n';
    std::cout << "[IOP loader regression] malformed, wrapped and colliding images preserve live allocations: "
        << (rollback && bounded && fixedProtected?"PASS":"FAIL") << '\n';
    return protectedImages && reused && rollback && bounded && fixedProtected?0:1;
}
