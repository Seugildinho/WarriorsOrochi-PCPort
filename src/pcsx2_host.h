#pragma once
// The port's side of PCSX2's GS (src/pcsx2_host.cpp, src/pcsx2_gs_bridge.cpp).
// Plain types only, so port code can include it without the PCSX2 headers.
#include <cstdint>

namespace OrochiPcsx2Host
{
    // The window the GS presents to (a Win32 HWND) and its client size.
    void SetRenderWindow(void *hwnd, unsigned width, unsigned height);
}

namespace OrochiPcsx2Gs
{
    // The privileged GS registers the display reads at a VBlank.
    struct Registers
    {
        uint64_t pmode, smode1, smode2, srfsh, synch1, synch2, syncv;
        uint64_t dispfb1, display1, dispfb2, display2, bgcolor, csr;
    };

    // Starts PCSX2's GS (Direct3D 11) on its own thread, presenting to `hwnd`.
    // `resources` holds shaders/dx11. `widescreen` shows the picture at 16:9
    // (for the game's widescreen patch) instead of 4:3. False when the GS
    // could not open.
    bool Start(const char *resources, void *hwnd, unsigned width, unsigned height, unsigned upscale,
               bool widescreen);
    // GIF packet bytes in arrival order (any split; whole quadwords are sent on).
    void Packet(const uint8_t *data, uint32_t size);
    // A VBlank: shows the frame. At most two wait queued; the caller blocks
    // beyond that, so the game cannot run ahead of the GS.
    void VSync(const Registers &registers, bool oddField);
    void Resize(unsigned width, unsigned height);
    void Stop();

    // Starts a frame capture (OROCHI_GIF_CAPTURE); false when none is configured.
    bool RequestCapture();
}
