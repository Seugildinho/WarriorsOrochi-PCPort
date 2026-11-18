#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include <iostream>
#include <memory>

namespace {
constexpr uint32_t startPc=0x100100,donePc=0x100104,watchdogPc=0x100108;
unsigned ticks{};
uint32_t initialField{};
bool progressive{},completed{};
void watchdog(uint8_t *,R5900Context *ctx,PS2Runtime *runtime) {
    if(++ticks>=8) runtime->requestStop();
    ctx->pc=0;
}
void start(uint8_t *,R5900Context *ctx,PS2Runtime *) {
    ctx->r[4]=_mm_set_epi64x(0,progressive?0:initialField);
    ctx->r[31]=_mm_set_epi64x(0,donePc);
    // The actual movie handshake waits until sceGsSyncV changes field, then
    // sets its own ready flag. No test callback changes that flag.
    ctx->pc=progressive?0x1353f0:0x117978;
}
void done(uint8_t *,R5900Context *ctx,PS2Runtime *runtime) {
    completed=progressive?getRegU32(ctx,2)==1:runtime->memory().read32(0x800000+0x912c0)==1;
    ctx->pc=0;runtime->requestStop();
}
bool test(bool progressiveMode,uint32_t field) {
    progressive=progressiveMode;initialField=field;ticks=0;completed=false;
    auto runtime=std::make_unique<PS2Runtime>();
    if(!runtime->memory().initialize())return false;
    runtime->configureGuestHeap(0x1f00000,0x1f00000);
    runtime->memory().gs().smode2=progressive?0:3;
    runtime->memory().write32(0x358208,0x800000);
    runtime->registerFunction(startPc,start);runtime->registerFunction(donePc,done);
    runtime->registerFunction(watchdogPc,watchdog);
    R5900Context ctx{};ctx.pc=startPc;
    ctx.r[28]=_mm_set_epi64x(0,0x360170);ctx.r[29]=_mm_set_epi64x(0,0x9000);
    runtime->eeScheduler().reset(runtime->memory().getRDRAM(),ctx);
    runtime->eeScheduler().setGsVSyncCallback(watchdogPc,0x360170,0);
    runtime->eeScheduler().run();
    return completed && ticks<8;
}
}

int runSyncVRegressionTests() {
    const bool first=test(false,0),second=test(false,1),progressiveMode=test(true,0);
    std::cout << "[SyncV regression] original movie handshake completes from either field with guest-programmed SMODE2: "
        << (first && second?"PASS":"FAIL") << '\n';
    std::cout << "[SyncV regression] progressive mode retains the SDK return value: " << (progressiveMode?"PASS":"FAIL") << '\n';
    return first && second && progressiveMode?0:1;
}
