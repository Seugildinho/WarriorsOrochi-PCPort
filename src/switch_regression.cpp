#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <array>
#include <iostream>
#include <memory>

int runSwitchRegressionTests() {
    auto runtime=std::make_unique<PS2Runtime>();
    if(!runtime->memory().initialize()) return 2;
    runtime->setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
    auto *ram=runtime->memory().getRDRAM();
    // Exact eight-way table at 0x3387b0, verified from the original USA ELF.
    const std::array<uint32_t,8> targets={0x270de8,0x270de8,0x270de8,0x270de8,
                                        0x270e00,0x270e00,0x270e18,0x270e18};
    unsigned passed=0;
    for(unsigned index=0;index<targets.size();index++) for(bool equal:{false,true}) {
        const auto target=targets[index];
        if(!runtime->hasFunction(target)) {
            std::cerr << "[Switch regression] missing case 0x" << std::hex << target << std::dec << '\n';
            return 1;
        }
        R5900Context ctx{};
        auto reg=[&](unsigned number,uint32_t value) {ctx.r[number]=_mm_set_epi64x(0,value);};
        reg(3,9); reg(4,12); reg(5,96); reg(6,70); reg(7,15);
        reg(15,equal?15:35); reg(17,index); reg(24,0x5000);
        reg(29,0x9000); reg(31,0x26ffb4);
        runtime->memory().write32(0x503c,0xdeadbeef);
        ctx.pc=target;
        runtime->lookupFunction(target)(ram,&ctx,runtime.get());
        const uint32_t expected=equal?(index<4?0:index<6?96:12):(index<6?70:9);
        const bool ok=runtime->memory().read32(0x503c)==expected && !runtime->isStopRequested() &&
            getRegU32(&ctx,29)==0x9000 && getRegU32(&ctx,31)==0x26ffb4;
        passed+=ok;
        if(!ok) std::cerr << "[Switch regression] wrong case index=" << index << " equal=" << equal << '\n';
    }
    std::cout << "[Switch regression] all eight entries execute their own case, both branches: "
        << (passed==16?"PASS":"FAIL") << " cases=" << passed << "/16\n";
    return passed==16?0:1;
}
