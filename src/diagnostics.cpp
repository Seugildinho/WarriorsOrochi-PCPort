#include "diagnostics.h"
#include "native_image.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "../vendor/PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Pad.h"
#include "raylib.h"
#include "rlgl.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <map>
#include <stdexcept>
#include <cstring>

namespace {
unsigned frames = 0;
bool pressStartTest=false;
std::map<unsigned,uint16_t> scriptedButtons;
std::filesystem::path captureDirectory;
std::filesystem::path controlFile;
uint64_t lastControlSequence=0;
unsigned releaseAtFrame=0;
std::chrono::steady_clock::time_point lastIntervalWall{};
uint64_t lastVsyncTick=0;
OrochiImagePerfSnapshot lastImagePerf{};
uint64_t lastIopInstructions=0;
bool diagDumps=false;
bool detailedDiagnostics=true;
void drawDiagnostics(PS2Runtime &runtime, void *) {
    const auto diagStart = std::chrono::steady_clock::now();
    ++frames;
    if(releaseAtFrame && frames>=releaseAtFrame) {
        ps2_stubs::setPadOverrideState(0xffffu,128,128,128,128);
        releaseAtFrame=0;
    }
    if(pressStartTest && frames==600) {
        ps2_stubs::setPadOverrideState(0xfff7u,128,128,128,128);
        std::cout << "[Orochi:test-input] START pressed\n";
    }
    if(pressStartTest && frames==630) {
        ps2_stubs::clearPadOverrideState();
        std::cout << "[Orochi:test-input] START released\n";
    }
    if(const auto event=scriptedButtons.find(frames);event!=scriptedButtons.end()) {
        ps2_stubs::setPadOverrideState(event->second,128,128,128,128);
        std::cout << "[Orochi:test-input] frame=" << frames << " buttons=0x" << std::hex
                  << event->second << std::dec << '\n';
    }
    if(!controlFile.empty() && frames%6==0) {
        std::ifstream file(controlFile);
        uint64_t sequence{}; std::string command; unsigned duration{};
        if(file>>sequence>>command>>duration && sequence>lastControlSequence) {
            if(command=="STOP") {
                lastControlSequence=sequence;
                std::cout << "[Orochi:test-input] control STOP frame=" << frames << '\n';
                runtime.requestStop(); return;
            }
            std::istringstream value(command); uint32_t buttons{};
            if(value>>buttons && value.eof() && buttons<=0xffff && duration>=1 && duration<=120) {
                lastControlSequence=sequence;
                // Optional left stick (lx ly, 0..255) after the duration.
                unsigned lx=128,ly=128;
                if(!(file>>lx>>ly) || lx>255 || ly>255) lx=ly=128;
                ps2_stubs::setPadOverrideState(uint16_t(buttons),uint8_t(lx),uint8_t(ly),128,128);
                releaseAtFrame=frames+duration;
                std::cout << "[Orochi:test-input] control frame=" << frames << " buttons=0x"
                    << std::hex << buttons << std::dec << " duration=" << duration << '\n';
            }
        }
    }
    uint64_t pngMicroseconds = 0;
    // 2026-10-23: periodic boot-frame.png export (~21-31 ms each) only when a probe
    // asks for it (OROCHI_DIAG_DUMPS, set by tools/run_probe.py) or frames are captured.
    if ((diagDumps || !captureDirectory.empty()) && (frames == 1 || frames % 120 == 0)) {
        const auto pngStart = std::chrono::steady_clock::now();
        const auto screenshot = PS2Runtime::getIoPaths().elfDirectory.parent_path() / "relatorios/boot-frame.png";
        // This callback runs before EndDrawing; flush raylib's queued texture
        // draw before reading the framebuffer, otherwise only the clear is seen.
        rlDrawRenderBatchActive();
        const auto image = LoadImageFromScreen();
        ExportImage(image, screenshot.string().c_str());
        if(!captureDirectory.empty())
            ExportImage(image,(captureDirectory/("frame-"+std::to_string(frames)+".png")).string().c_str());
        UnloadImage(image);
        pngMicroseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pngStart).count());
    }
    const bool inputSample=(pressStartTest && (frames==615 || frames==645)) ||
        (frames>15 && scriptedButtons.count(frames-15));
    if(frames%120==0 || inputSample) {
        const auto pad=ps2_stubs::getPadDebugSnapshot();
        for(unsigned port=0;port<2;port++) {
            const auto &state=pad.ports[port][0];
            // USA controller structures: base loaded by 0x10430c, stride 100.
            const auto base=0x003fb980u+port*100u;
            std::cout << "[Orochi:pad-state] frame=" << frames << " port=" << port
                << " reads=" << state.readCount << " open=" << state.open << " analog=" << state.analogMode
                << " override=" << state.lastUsedOverride << " mask=0x" << std::hex << state.lastButtons
                << " mode=0x" << unsigned(state.lastData[1])
                << " flags=0x" << unsigned(runtime.memory().read8(base+0x63))
                << " held=0x" << runtime.memory().read16(base+0x48)
                << " pressed=0x" << runtime.memory().read16(base+0x4c) << std::dec << '\n';
        }
    }
    if (frames % 120) return;
    const auto menu=runtime.memory().read32(0x003589ccu);
    if(menu && menu<PS2_RAM_SIZE-0x50) {
        std::cout << "[Orochi:menu-state] frame=" << frames << " data=0x" << std::hex << menu << std::dec
            << " intro=" << runtime.memory().read32(menu) << " introTimer=" << runtime.memory().read32(menu+4)
            << " wheel=" << runtime.memory().read32(menu+0xc) << " wheelTimer=" << runtime.memory().read32(menu+0x10)
            << " selection=" << runtime.memory().read32(menu+0x14) << '\n';
    }
    if(detailedDiagnostics) {
    const auto graphics=runtime.gs().getDebugSnapshot();
    std::cout << "[Orochi:display-source] preferred=" << graphics.hasPreferredDisplaySource
        << " sourceFbp=" << graphics.preferredDisplaySourceFrame.fbp
        << " destFbp=" << graphics.preferredDisplayDestFbp
        << " hostSourceFbp=" << graphics.hostPresentationSourceFbp
        << " hostDisplayFbp=" << graphics.hostPresentationDisplayFbp
        << " usedPreferred=" << graphics.hostPresentationUsedPreferred << '\n';
    const auto &registers=runtime.memory().gs();
    std::cout << "[Orochi:display-registers] pmode=0x" << std::hex << registers.pmode
        << " smode2=0x" << registers.smode2 << " fb1=0x" << registers.dispfb1
        << " fb2=0x" << registers.dispfb2 << " display1=0x" << registers.display1
        << " display2=0x" << registers.display2 << std::dec << '\n';
    std::vector<uint8_t> presentation; uint32_t width{},height{};
    if(runtime.gs().copyLatchedHostPresentationFrame(presentation,width,height)) {
        unsigned colors=0,alpha=0;
        for(size_t i=0;i+3<presentation.size();i+=4) {
            colors+=(presentation[i]||presentation[i+1]||presentation[i+2]);
            alpha+=presentation[i+3]!=0;
        }
        std::cout << "[Orochi:presentation] size=" << width << 'x' << height
            << " colorPixels=" << colors << " alphaPixels=" << alpha << '\n';
    }
    std::cout << "[Orochi:gs] frame=" << frames << " prim=" << unsigned(graphics.prim.type)
        << " context=" << unsigned(graphics.prim.ctxt) << " textured=" << graphics.prim.tme
        << " transfer=" << graphics.transferCopiedPixels << '/' << graphics.transferTotalPixels << '\n';
    for(unsigned i=0;i<2;i++) {
        const auto &context=graphics.ctx[i];
        std::cout << "[Orochi:gs-context] id=" << i << " fbp=" << context.frame.fbp
            << " fbw=" << context.frame.fbw << " psm=" << unsigned(context.frame.psm)
            << " mask=0x" << std::hex << context.frame.fbmsk << std::dec
            << " texture=" << context.tex0.tbp0 << " tbw=" << unsigned(context.tex0.tbw)
            << " size=" << (1u<<context.tex0.tw) << 'x' << (1u<<context.tex0.th)
            << " offset=" << context.xyoffset.ofx << ',' << context.xyoffset.ofy
            << " scissor=" << context.scissor.x0 << ',' << context.scissor.y0 << ','
            << context.scissor.x1 << ',' << context.scissor.y1 << '\n';
    }
    const auto history=runtime.gs().getDebugHistory();
    for(auto it=history.rbegin();it!=history.rend();++it) if(it->kind==GSDebugEventKind::Draw) {
        std::cout << "[Orochi:gs-draw] vertices=" << it->vertexCount << " x=" << it->xMin << ',' << it->xMax
            << " y=" << it->yMin << ',' << it->yMax << " alpha=" << unsigned(it->aMin) << ',' << unsigned(it->aMax) << '\n';
        std::cout << "[Orochi:gs-draw-state] prim=" << unsigned(it->prim.type) << " fbp=" << it->frame.fbp
            << " tex=" << it->tex0.tbp0 << " test=0x" << std::hex << it->test
            << " blend=0x" << it->alpha << std::dec << '\n';
        break;
    }
    if(diagDumps && frames%600==0) {
        runtime.gs().refreshDisplaySnapshot();
        uint32_t bytes{};
        const auto *vram=runtime.gs().lockDisplaySnapshot(bytes);
        if(vram && bytes) {
            std::ofstream file(PS2Runtime::getIoPaths().elfDirectory/"gpu-vram.bin",std::ios::binary);
            file.write(reinterpret_cast<const char *>(vram),bytes);
        }
        runtime.gs().unlockDisplaySnapshot();
    }
    const auto snapshot = runtime.eeScheduler().snapshot();
    std::cout << "[Orochi:kernel] frame=" << frames << " sequence=" << snapshot.sequence
              << " cycle=" << snapshot.eeCycle << " running=" << snapshot.runningThreadId << '\n';
    for (const auto &thread : snapshot.threads) {
        std::cout << "[Orochi:thread] id=" << thread.id << " entry=0x" << std::hex << thread.entry
                  << " pc=0x" << thread.pc << " ra=0x" << thread.ra << " sp=0x" << thread.sp
                  << std::dec << " status=" << int(thread.status) << " wait=" << int(thread.waitReason)
                  << " waitId=" << thread.waitId << " priority=" << thread.currentPriority
                  << " callbacks=" << thread.invocationDepth << '\n';
        if(thread.id==1 && std::getenv("OROCHI_CAPTURE_RAM")) {
            std::cout << "[Orochi:main-stack] frame=" << frames << " sp=0x" << std::hex << thread.sp;
            for(unsigned offset=0;offset<0x100;offset+=4) {
                const auto address=thread.sp+offset;
                const auto *data=getConstMemPtr(runtime.memory().getRDRAM(),address);
                uint32_t value{};
                if(data && getConstMemPtr(runtime.memory().getRDRAM(),address+3)==data+3)
                    std::memcpy(&value,data,4);
                std::cout << " w" << offset << "=0x" << value;
            }
            std::cout << std::dec << '\n';
        }
    }
    for (const auto &semaphore : snapshot.semaphores) {
        std::cout << "[Orochi:semaphore] id=" << semaphore.id << " count=" << semaphore.count
                  << " max=" << semaphore.maxCount << " waiters=" << semaphore.waiters << '\n';
    }
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wallUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - lastIntervalWall).count());
    const auto diagUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - diagStart).count());
    const auto curVsync = runtime.eeScheduler().currentVSyncTick();
    const auto curImage = getOrochiImagePerfSnapshot();
    const auto curIop = runtime.iopDebugSnapshot().emulatorInstructions;
    std::cout << "[Orochi:perf] frame=" << frames
              << " wallMs=" << (wallUs / 1000.0)
              << " vsyncDelta=" << (curVsync - lastVsyncTick)
              << " ipuFrames=" << (curImage.decodedFrames - lastImagePerf.decodedFrames)
              << " ipuTotalMs=" << ((curImage.totalMicroseconds - lastImagePerf.totalMicroseconds) / 1000.0)
              << " ipuProcMs=" << ((curImage.processMicroseconds - lastImagePerf.processMicroseconds) / 1000.0)
              << " ipuCacheHits=" << (curImage.cacheHits - lastImagePerf.cacheHits)
              << " pngMs=" << (pngMicroseconds / 1000.0)
              << " diagMs=" << (diagUs / 1000.0)
              << " iopInstrDelta=" << (curIop - lastIopInstructions) << '\n';
    lastIntervalWall = now;
    lastVsyncTick = curVsync;
    lastImagePerf = curImage;
    lastIopInstructions = curIop;
}
}

void installOrochiDiagnostics(PS2Runtime &runtime) {
    const bool requested=std::getenv("OROCHI_DIAG_DUMPS") || std::getenv("OROCHI_CAPTURE_DIR") ||
        std::getenv("OROCHI_TEST_CONTROL_FILE") || std::getenv("OROCHI_TEST_INPUTS") ||
        std::getenv("OROCHI_TEST_START") || std::getenv("OROCHI_CAPTURE_RAM") || std::getenv("OROCHI_DIAGNOSTICS");
    if(!requested) {
        runtime.setDebugUiCallbacks([](PS2Runtime &rt,void *) {rt.gs().setDebugHistoryPaused(true);},nullptr,nullptr,nullptr);
        return;
    }
    runtime.setDebugUiCallbacks([](PS2Runtime &runtime, void *) {
        frames=0; pressStartTest=std::getenv("OROCHI_TEST_START")!=nullptr;
        scriptedButtons.clear(); captureDirectory.clear(); controlFile.clear();
        lastControlSequence=0; releaseAtFrame=0;
        lastIntervalWall=std::chrono::steady_clock::now();
        lastVsyncTick=0; lastImagePerf={}; lastIopInstructions=0;
        diagDumps=std::getenv("OROCHI_DIAG_DUMPS")!=nullptr;
        detailedDiagnostics=std::getenv("OROCHI_LIGHTWEIGHT_DIAGNOSTICS")==nullptr;
        if(const auto path=std::getenv("OROCHI_TEST_CONTROL_FILE")) controlFile=path;
        if(const auto text=std::getenv("OROCHI_TEST_INPUTS")) {
            std::istringstream input(text); std::string entry;
            while(std::getline(input,entry,',')) {
                const auto colon=entry.find(':');
                if(colon==std::string::npos) throw std::runtime_error("Invalid scripted input");
                const auto frame=std::stoul(entry.substr(0,colon));
                const auto buttons=std::stoul(entry.substr(colon+1));
                if(!frame || frame>18000 || buttons>0xffff) throw std::runtime_error("Scripted input out of range");
                scriptedButtons.emplace(unsigned(frame),uint16_t(buttons));
            }
        }
        if(const auto path=std::getenv("OROCHI_CAPTURE_DIR")) {
            captureDirectory=path; std::filesystem::create_directories(captureDirectory);
        }
        runtime.gs().setDebugHistoryPaused(!detailedDiagnostics);
    },drawDiagnostics,[](PS2Runtime &, void *) { ps2_stubs::clearPadOverrideState(); },nullptr);
}
