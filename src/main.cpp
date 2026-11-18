#include "ps2_runtime.h"
#include "native_cd.h"
#include "native_iop.h"
#include "diagnostics.h"
#include "graphics_trace.h"
#include "native_image.h"
#include "native_pad.h"
#include "native_overlay.h"
#include "native_sif.h"
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <vector>

int runDmaRegressionTests();
int runVSyncRegressionTests();
int runGifRegressionTests();
int runDisplayRegressionTests();
int runSwitchRegressionTests();
int runIopMemoryRegressionTests();
int runIopLoaderRegressionTests();
int runSyncVRegressionTests();
int runMpegAudioRegressionTests();
#include "native_mpeg.h"
#include "native_gpu.h"
#include "native_vu1.h"
#include "gs_thread_backend.h"
#include "game_patches.h"
#include "auto_save.h"
#include "cheats.h"
#include "draw_distance.h"
#include "disc_image.h"
#include "game_settings.h"
#include "orochi_settings.h"
#ifdef OROCHI_PCSX2_GS
#include "pcsx2_runtime.h"
#endif
#include "runtime/gs/gs_cpu_backend.h"

// Native EE prototype. Game data stays on the user's original local disc image.
int main(int argc, char **argv)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    if (argc == 2 && std::string(argv[1]) == "--test-dma") return runDmaRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-vsync") return runVSyncRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-gif") return runGifRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-display") return runDisplayRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-pad") return runPadRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-overlay") return runOverlayRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-switch") return runSwitchRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-sif") return runSifRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-iop-memory") return runIopMemoryRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-iop-loader") return runIopLoaderRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-syncv") return runSyncVRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-mpeg-audio") return runMpegAudioRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-mpeg-transport") return runMpegTransportRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-gpu") return runGpuRegressionTests();
    if (argc == 2 && std::string(argv[1]) == "--test-gs-thread") return runGsThreadRegressionTests();
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: WarriorsOrochi [<SLUS_216.62> <disc.iso>]\n";
        return 2;
    }
    try {
        // Without arguments the game starts as the setup program left it.
        // The tools name their own ELF and disc, and set their own options.
        const bool fromSettings = argc == 1;
        const OrochiSettings settings = fromSettings ? orochiLoadSettings() : OrochiSettings{};
        std::filesystem::path iso, gameElf;
        std::string discError;
        if (fromSettings) {
            // The memory cards are found from the ELF's folder.
            const auto data = orochiProgramFolder() / "data";
            std::filesystem::create_directories(data);
            std::filesystem::current_path(data);
            orochiLogToFile(data / "port.log");
            iso = orochiPathFromUtf8(settings.disc);
            gameElf = data / "SLUS_216.62";
            if (iso.empty() || !orochiExtractDiscElf(iso, gameElf, discError)) {
                std::cerr << "[Orochi] No usable disc configured. " << discError << '\n';
                return orochiRunProgram(kOrochiSetupProgram) ? 0 : 2;
            }
        } else {
            const auto elf = std::filesystem::absolute(argv[1]);
            iso = std::filesystem::absolute(argv[2]);
            if (!std::filesystem::is_regular_file(elf) || !std::filesystem::is_regular_file(iso)) {
                std::cerr << "The private game ELF or original disc image is missing.\n";
                return 2;
            }
            gameElf = elf.parent_path() / "disc-selected.elf";
            if (!orochiExtractDiscElf(iso, gameElf, discError)) {
                std::cerr << "[Orochi] " << discError << '\n';
                return 4;
            }
        }
        PS2Runtime runtime;
        installOrochiDiagnostics(runtime);
        runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
        std::cout << "[Orochi] Experimental native EE build; gameplay is unverified.\n";
        if (!runtime.initialize("Warriors Orochi | Native PS2 prototype")) {
            std::cerr << "[Orochi] Window/runtime initialization failed.\n";
            return 3;
        }
        showOrochiWindow();
        if (fromSettings) applyOrochiSettings(settings);
        auto paths = PS2Runtime::getIoPaths();
        paths.cdImage = iso;
        PS2Runtime::setIoPaths(paths);
        std::cout << "[Orochi] ELF: " << gameElf << "\n[Orochi] Disc: " << iso << '\n';
        if (!runtime.loadELF(gameElf.string())) {
            std::cerr << "[Orochi] ELF loading failed.\n";
            return 4;
        }
        const auto renderer=std::getenv("OROCHI_RENDERER");
        const std::string rendererMode=renderer ? renderer : "auto";
        std::unique_ptr<GSRasterBackend> raster;
        if (OrochiGamePatches::widescreen && rendererMode!="pcsx2") {
            // The own renderers present the 640x448 frame at its own aspect.
            std::cerr << "[Orochi] OROCHI_WIDESCREEN needs OROCHI_RENDERER=pcsx2; showing 4:3.\n";
            OrochiGamePatches::widescreen=false;
        }
        if (rendererMode=="pcsx2") {
#ifdef OROCHI_PCSX2_GS
            if(!installOrochiPcsx2Gs(runtime)) {
                std::cerr << "[Orochi] PCSX2 GS renderer could not start.\n";
                return 6;
            }
#else
            std::cerr << "[Orochi] This build has no PCSX2 GS (configure with OROCHI_PCSX2_GS=ON).\n";
            return 6;
#endif
        }
        if (rendererMode=="auto" || rendererMode=="d3d11") {
            std::string error;
            raster=createOrochiGpuBackend(error);
            if(!raster) {
                std::cerr << "[Orochi] Hardware renderer unavailable: " << error << '\n';
                if(rendererMode=="d3d11") return 6;
                std::cerr << "[Orochi] Continuing with the CPU renderer.\n";
            }
        }
        // GS rasterization runs on its own thread unless OROCHI_GS_THREAD=0;
        // queued commands keep their order and every read drains the queue.
        const auto gsThread=std::getenv("OROCHI_GS_THREAD");
        if (rendererMode!="pcsx2" && (!gsThread || std::string(gsThread)!="0")) {
            if(!raster) raster=std::make_unique<GSCpuBackend>();
            raster=createOrochiThreadedBackend(std::move(raster));
            std::cout << "[Orochi] GS rasterization runs on a separate thread.\n";
        }
        if(raster) runtime.gs().setRasterBackend(std::move(raster));
        if (!installOrochiDiscFunctions(runtime) || !installOrochiSifFunctions(runtime) || !installOrochiIopFunctions(runtime) ||
            !installOrochiGraphicsTrace(runtime) || !installOrochiImageFunctions(runtime) ||
            !installOrochiPadFunctions(runtime) || !installOrochiSupplementalFunctions(runtime) || !installOrochiOverlayFunctions(runtime) ||
            !installOrochiMpegFunctions(runtime) || !installOrochiAutoSave(runtime) || !installOrochiCheats(runtime) ||
            !installOrochiDrawDistance(runtime)) {
            std::cerr << "[Orochi] Could not bind native game adapters.\n";
            return 5;
        }
        installOrochiVu1Translations();
        // VIF1 DMA (VU1 microprograms and their GIF output) runs on its own
        // thread in parallel with the EE; OROCHI_VIF1_THREAD=0 keeps it inline.
        {
            const char *vif1Thread = std::getenv("OROCHI_VIF1_THREAD");
            const bool asyncVif1 = !vif1Thread || std::string(vif1Thread) != "0";
            runtime.enableAsyncVif1(asyncVif1);
            std::cout << "[Orochi:vif1] async=" << asyncVif1 << '\n';
        }
        std::cout << "[Orochi] Entering recompiled PS2 code.\n";
        runtime.run();
        if(std::getenv("OROCHI_CAPTURE_RAM")) {
            const auto directory=gameElf.parent_path();
            std::ofstream dump(directory/"ee-memory-after-stop.bin",std::ios::binary);
            dump.write(reinterpret_cast<const char *>(runtime.memory().getRDRAM()),PS2_RAM_SIZE);
            std::ofstream scratch(directory/"ee-scratchpad-after-stop.bin",std::ios::binary);
            scratch.write(reinterpret_cast<const char *>(runtime.memory().getScratchpad()),PS2_SCRATCHPAD_SIZE);
            std::ofstream context(directory/"ee-context-after-stop.txt");
            context << "pc=0x" << std::hex << runtime.cpu().pc << '\n';
            for(unsigned reg=0;reg<32;reg++) context << 'r' << std::dec << reg << "=0x" << std::hex
                << getRegU32(&runtime.cpu(),reg) << '\n';
            std::vector<uint8_t> iopRam(2*1024*1024);
            if(runtime.readIopMemory(0,iopRam.data(),iopRam.size())) {
                std::ofstream iopDump(directory/"iop-memory-after-stop.bin",std::ios::binary);
                iopDump.write(reinterpret_cast<const char *>(iopRam.data()),iopRam.size());
            }
            const auto iop=runtime.iopDebugSnapshot();
            std::ofstream iopState(directory/"iop-state-after-stop.txt");
            iopState << "cycles=" << iop.emulatorCycles << " instructions=" << iop.emulatorInstructions
                << " modules=" << iop.emulatorLoadedModules << " threads=" << iop.emulatorThreads
                << " rpcServers=" << iop.emulatorRpcServers << '\n';
            for(const auto &message:iop.diagnostics) iopState << message << '\n';
            for(const auto &service:iop.services) {
                iopState << "service=" << service.name << " active=" << service.active << '\n';
                for(const auto &metric:service.metrics)
                    iopState << "  " << metric.name << '=' << metric.value << '\n';
            }
            std::cout << "[Orochi:diagnostic] EE RAM captured after runtime stop\n";
        }
        std::cout << "[Orochi] Runtime stopped; inspect logs for guest errors.\n";
        runtime.gs().setRasterBackend(nullptr);
        // Matches upstream shutdown: some PS2 subsystems keep background threads.
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    } catch (const std::exception &error) {
        std::cerr << "[Orochi] Fatal exception: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[Orochi] Unknown fatal exception.\n";
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
