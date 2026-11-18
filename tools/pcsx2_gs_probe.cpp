// Bring-up check for PCSX2's GS inside the port (relatorios/pcsx2-gs-integracao.md):
// opens a window, starts the GS with Direct3D 11, sets up a 640x448 display
// and draws a red sprite on a blue frame through a GIF packet for a few
// seconds. usage: pcsx2_gs_probe <thirdparty/pcsx2/bin/resources>
#include "GS/GS.h"
#include "Config.h"
#include "pcsx2_host.h"
#include "common/RedtapeWindows.h"

#include <dbghelp.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace
{
    // Prints the faulting stack with symbols (the probe runs without a debugger).
    LONG WINAPI crashed(EXCEPTION_POINTERS *info)
    {
        std::fprintf(stderr, "exception 0x%08lx at %p\n", info->ExceptionRecord->ExceptionCode,
                     info->ExceptionRecord->ExceptionAddress);
        HANDLE process = GetCurrentProcess();
        SymInitialize(process, nullptr, TRUE);
        CONTEXT context = *info->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = context.Rip;
        frame.AddrStack.Offset = context.Rsp;
        frame.AddrFrame.Offset = context.Rbp;
        frame.AddrPC.Mode = frame.AddrStack.Mode = frame.AddrFrame.Mode = AddrModeFlat;
        for (int depth = 0; depth < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame,
                                                      &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++depth)
        {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            IMAGEHLP_LINE64 line{sizeof(line)};
            DWORD lineDisplacement = 0;
            const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
            const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line);
            std::fprintf(stderr, "  %s+0x%llx %s:%lu\n", named ? symbol->Name : "?", displacement,
                         lined ? line.FileName : "", lined ? line.LineNumber : 0ul);
        }
        std::fflush(stderr);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_DESTROY)
            PostQuitMessage(0);
        return DefWindowProcW(window, message, w, l);
    }

    // A+D (PACKED, register 0xE) entries: value then register address.
    void ad(std::vector<u64> &packet, u64 reg, u64 value)
    {
        packet.push_back(value);
        packet.push_back(reg);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: pcsx2_gs_probe <bin/resources>\n");
        return 2;
    }
    SetUnhandledExceptionFilter(crashed);
    std::printf("probe start\n");
    std::fflush(stdout);
    EmuFolders::Resources = argv[1];
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OrochiPcsx2GsProbe";
    RegisterClassW(&windowClass);
    RECT rect{0, 0, 640, 448};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowW(windowClass.lpszClassName, L"PCSX2 GS probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                windowClass.hInstance, nullptr);
    OrochiPcsx2Host::SetRenderWindow(window, 640, 448);

    alignas(16) static u8 privileged[0x2000]{};
    auto setPrivileged = [](u32 offset, u64 value) { std::memcpy(privileged + offset, &value, 8); };
    Pcsx2Config::GSOptions options;
    options.Renderer = GSRendererType::DX11;
    if (!GSopen(options, GSRendererType::DX11, privileged, GSVSyncMode::Disabled, false))
    {
        std::fprintf(stderr, "GSopen failed\n");
        return 1;
    }
    std::printf("GSopen ok\n");
    // PMODE: circuit 1, ALP=0xFF. SMODE2: interlaced frame mode. DISPFB1: FBW=10, CT32.
    // DISPLAY1: DX=636, DY=50, MAGH=3, DW=2559, DH=447.
    setPrivileged(0x0000, 0x0000FF01ull | (1ull << 5));
    setPrivileged(0x0010, 2ull << 13);  // SMODE1: CMOD=2 (NTSC)
    setPrivileged(0x0020, 0x3ull);
    setPrivileged(0x0070, (10ull << 9));
    setPrivileged(0x0080, 636ull | (50ull << 12) | (3ull << 23) | (2559ull << 32) | (447ull << 44));

    std::vector<u64> packet;
    // GIFtag: NLOOP set below, EOP, PACKED, one register descriptor A+D (0xE).
    packet.push_back(0);
    packet.push_back(0xEull);
    ad(packet, 0x4C, 10ull << 16);                                // FRAME_1: FBP 0, FBW 10, CT32
    ad(packet, 0x40, 639ull << 16 | 447ull << 48);                // SCISSOR_1
    ad(packet, 0x18, 0);                                          // XYOFFSET_1
    ad(packet, 0x47, 1ull << 16 | 1ull << 17);                    // TEST_1: ZTE, ZTST always
    ad(packet, 0x4E, 1ull << 32);                                 // ZBUF_1: ZMSK (no depth writes)
    ad(packet, 0x00, 6);                                          // PRIM: sprite
    ad(packet, 0x01, 0x80u << 24 | 0xC0u << 16 | 0x40u << 8 | 0x20u);  // RGBAQ: blue
    ad(packet, 0x05, 0);                                          // XYZ2 (0,0)
    ad(packet, 0x05, (640ull << 4) | (448ull << 4) << 16);        // XYZ2 (640,448)
    ad(packet, 0x01, 0x80u << 24 | 0x20u << 16 | 0x20u << 8 | 0xE0u);  // RGBAQ: red
    ad(packet, 0x05, (160ull << 4) | (112ull << 4) << 16);
    ad(packet, 0x05, (480ull << 4) | (336ull << 4) << 16);
    const u64 nloop = (packet.size() - 2) / 2;
    packet[0] = nloop | (1ull << 15) | (1ull << 60);

    for (int frame = 0; frame < 1500; ++frame)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
                frame = 1500;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        GSgifTransfer(reinterpret_cast<const u8 *>(packet.data()), static_cast<u32>(packet.size() / 2));
        GSvsync(frame & 1, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    GSclose();
    std::printf("done\n");
    return 0;
}
