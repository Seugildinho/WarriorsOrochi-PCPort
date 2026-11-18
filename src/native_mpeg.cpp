#include "native_mpeg.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/MPEG.h"
#include <cstring>
#include <iostream>
#include <memory>

namespace {
unsigned receiptLogs{};
void receiveVideo(uint8_t *,R5900Context *ctx,PS2Runtime *runtime) {
    const auto mpeg=getRegU32(ctx,4),data=getRegU32(ctx,5);
    if((data&3u) || data>PS2_RAM_SIZE-32u ||
       !ps2_stubs::acknowledgeMpegNativeVideo(mpeg,runtime->memory().read32(data+8),
                                             runtime->memory().read32(data+12))) {
        std::cerr << "[Orochi:blocker] Native MPEG input receipt does not match the video callback\n";
        runtime->requestStop();return;
    }
    // Demux already copied these elementary bytes into the native decoder.
    // Do not enqueue the same payload again in the physical IPU DMA ring.
    if(receiptLogs++<4) std::cout << "[Orochi:mpeg-native] accepted bytes="
        << runtime->memory().read32(data+12) << '\n';
    setReturnS32(ctx,1);ctx->pc=getRegU32(ctx,31);
}
void finishVideo(uint8_t *,R5900Context *ctx,PS2Runtime *runtime) {
    // Original 0x116b98 is called only after the producer has submitted the
    // movie's final byte. Its hardware path appends sequence_end to the IPU.
    const bool finished=ps2_stubs::finishMpegNativeInput(getRegU32(ctx,4),runtime);
    setReturnS32(ctx,finished?1:0);ctx->pc=getRegU32(ctx,31);
}
void isVideoEnded(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    ps2_stubs::sceMpegIsEnd(ram,ctx,runtime);ctx->pc=getRegU32(ctx,31);
}
void isVideoReferenceEmpty(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    ps2_stubs::sceMpegIsRefBuffEmpty(ram,ctx,runtime);ctx->pc=getRegU32(ctx,31);
}
constexpr uint32_t testMpeg=0x8000,testMainPc=0x100100;
unsigned testStage{};
bool testPassed{};
void testMain(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    auto query=[&](uint32_t pc) {
        R5900Context call{};call.pc=pc;call.r[4]=_mm_set_epi64x(0,testMpeg);
        runtime->lookupFunction(pc)(ram,&call,runtime);return getRegU32(&call,2);
    };
    if(testStage==0) {
        testPassed=query(0x14b0c8)==0 && query(0x14b0d8)==0;
        testPassed &= query(0x116b98)==1;
        // EOF alone must not finish while an actual decoded frame is queued.
        testPassed &= query(0x14b0c8)==0 && query(0x14b0d8)==0;
        R5900Context picture{};picture.r[4]=_mm_set_epi64x(0,testMpeg);
        picture.r[5]=_mm_set_epi64x(0,0x20000);
        ps2_stubs::sceMpegGetPicture(ram,&picture,runtime);
        testPassed &= query(0x14b0d8)==1 && query(0x14b0c8)==0;
        testStage=1;
    }
    if(runtime->eeScheduler().currentVSyncTick()<3) {
        ctx->pc=testMainPc;
        runtime->eeScheduler().waitVSync(runtime->eeScheduler().currentVSyncTick());
    }
    testPassed &= query(0x14b0c8)==1;
    for(unsigned i=0;i<0x68;i++) testPassed &= ram[testMpeg+0x48+i]==0x5a;
    ctx->pc=0;runtime->requestStop();
}
}

bool installOrochiMpegFunctions(PS2Runtime &runtime) {
    receiptLogs=0;
    return runtime.replaceFunction(0x114ef0,receiveVideo) &&
        runtime.replaceFunction(0x116b98,finishVideo) &&
        runtime.replaceFunction(0x14b0c8,isVideoEnded) &&
        runtime.replaceFunction(0x14b0d8,isVideoReferenceEmpty);
}

int runMpegTransportRegressionTests() {
    auto runtime=std::make_unique<PS2Runtime>();
    if(!runtime->memory().initialize() || !installOrochiMpegFunctions(*runtime)) return 2;
    runtime->configureGuestHeap(0x1000000,0x1f00000);
    ps2_stubs::resetMpegStubState();
    auto *ram=runtime->memory().getRDRAM();
    std::memset(ram+testMpeg+0x48,0x5a,0x68);
    R5900Context registration{};registration.r[4]=_mm_set_epi64x(0,testMpeg);
    registration.r[7]=_mm_set_epi64x(0,0x114ef0);
    ps2_stubs::sceMpegAddStrCallback(ram,&registration,runtime.get());
    // No sequence header yet: the native parser owns this partial ES input.
    const uint8_t pes[]={0,0,1,0xe0,0,4,0x80,0,0,0x55};
    std::memcpy(ram+0x10000,pes,sizeof(pes));
    R5900Context main{};main.pc=testMainPc;main.r[29]=_mm_set_epi64x(0,0x9000);
    runtime->registerFunction(testMainPc,testMain);
    runtime->eeScheduler().reset(ram,main);
    R5900Context feed{};feed.r[4]=_mm_set_epi64x(0,testMpeg);
    feed.r[5]=_mm_set_epi64x(0,0x10000);feed.r[6]=_mm_set_epi64x(0,sizeof(pes));
    ps2_stubs::sceMpegDemuxPss(ram,&feed,runtime.get());
    const bool receiptProtected=!ps2_stubs::acknowledgeMpegNativeVideo(testMpeg,0x10009,2) &&
        !ps2_stubs::acknowledgeMpegNativeVideo(0x9000,0x10009,1) &&
        !ps2_stubs::finishMpegNativeInput(testMpeg,runtime.get());
    ps2_stubs::enqueueMpegDecodedFrameForTesting(testMpeg);
    testStage=0;testPassed=false;runtime->eeScheduler().run();
    const bool replayRejected=!ps2_stubs::acknowledgeMpegNativeVideo(testMpeg,0x10009,1);
    std::cout << "[MPEG transport regression] wrong/foreign/duplicate receipts rejected and EOF waits for acknowledgment: "
        << (receiptProtected && replayRejected?"PASS":"FAIL") << '\n';
    std::cout << "[MPEG transport regression] SDK end queries wait for decoded output and presentation; physical IPU ring unchanged: "
        << (testPassed?"PASS":"FAIL") << '\n';
    return receiptProtected && replayRejected && testPassed?0:1;
}
