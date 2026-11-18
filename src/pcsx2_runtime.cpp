// Wires PCSX2's GS (src/pcsx2_gs_bridge.cpp) to the runtime: the GS frontend's
// packet and VBlank taps go to it, and the frontend's raster backend becomes
// one that draws nothing.
#include "pcsx2_runtime.h"

#include "game_patches.h"
#include "pcsx2_host.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "raylib.h"
#include "game_settings.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace
{
    class NullRasterBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override
        {
            // GS-to-EE transfers would need PCSX2's local memory (not wired yet).
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                std::cerr << "[Orochi:pcsx2-gs] local-to-host transfer not supported\n";
            }
            return 0u;
        }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }
    };
}

void OrochiPcsx2Window::Resized(unsigned width, unsigned height)
{
    OrochiPcsx2Gs::Resize(width, height);
}

bool OrochiPcsx2Window::CaptureKey()
{
    return OrochiPcsx2Gs::RequestCapture();
}

bool installOrochiPcsx2Gs(PS2Runtime &runtime)
{
    const bool widescreen = OrochiGamePatches::widescreen;
    // A 16:9 window for the 16:9 picture, unless the setup screen placed it.
    if (widescreen && !orochiSetupPlacedWindow)
        SetWindowSize(960, 540);
    unsigned width = 0, height = 0;
    void *window = OrochiPcsx2Window::Create(GetWindowHandle(), width, height);
    if (!window)
        return false;
    unsigned upscale = 2;
    if (const char *scale = std::getenv("OROCHI_PCSX2_SCALE"))
        upscale = static_cast<unsigned>(std::max(1, std::atoi(scale)));
    const std::string resources = OrochiPcsx2Window::ExecutableDirectory() + "/pcsx2-resources";
    if (!OrochiPcsx2Gs::Start(resources.c_str(), window, width, height, upscale, widescreen))
        return false;
    runtime.gs().setRasterBackend(std::make_unique<NullRasterBackend>());
    runtime.gs().setPacketTap([](const uint8_t *data, uint32_t size) { OrochiPcsx2Gs::Packet(data, size); });
    runtime.gs().setVsyncTap([](const GSRegisters &r, uint64_t tick) {
        // Pauses the player would feel ("travadas"): a frame later than 100 ms.
        // Loading screens show here too; the log line carries the tick to tell.
        {
            using Clock = std::chrono::steady_clock;
            static Clock::time_point last{};
            static int reported = 0;
            const Clock::time_point now = Clock::now();
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            if (last != Clock::time_point{} && gap >= 100 && reported < 300)
            {
                ++reported;
                std::cout << "[Orochi:hitch] vsync=" << tick << " ms=" << gap << std::endl;
            }
            last = now;
        }
        // The BIOS's SetGsCrt writes SMODE1 and SYNCV, which the runtime leaves
        // at zero: PCSX2 reads CMOD 0 as VESA, and an even SYNCV.VFP as not
        // interlaced, so field-mode frames (SMODE2.FFMD, the pause menu) were
        // shown without their field offset and the gaps between fields showed
        // as dashed lines. These are the BIOS's NTSC interlaced values.
        const uint64_t smode1 = r.smode1 ? r.smode1 : 0x740834504ull;
        const uint64_t syncv = r.syncv ? r.syncv : 0xC7800601A01801ull;
        const OrochiPcsx2Gs::Registers registers{r.pmode, smode1, r.smode2, r.srfsh, r.synch1, r.synch2, syncv,
                                                 r.dispfb1, r.display1, r.dispfb2, r.display2, r.bgcolor,
                                                 r.csr.load(std::memory_order_relaxed)};
        OrochiPcsx2Gs::VSync(registers, (tick & 1u) != 0u);
    });
    std::cout << "[Orochi] PCSX2 GS renderer (Direct3D 11), internal scale " << upscale << "x, window "
              << width << 'x' << height << (widescreen ? ", 16:9" : ", 4:3") << '\n';
    return true;
}
