// Replays a GIF capture (OROCHI_GIF_CAPTURE, src/pcsx2_gs_bridge.cpp) through
// PCSX2's GS with the port's picture options and prints what a frame costs,
// without the game: the GS memory comes from <capture>.vram.
// usage: pcsx2_gs_replay <bin/resources> <capture> [frames] [key=value,...]
// keys: scale, fxaa, aniso, blend, present, mipmap, w, h
#include "GS/GS.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Common/GSRenderer.h"
#include "GS/Renderers/DX11/GSDevice11.h"
#include "Config.h"
#include "pcsx2_host.h"
#include "common/RedtapeWindows.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern bool g_orochi_sprite_edge_inset; // GSRendererHW.cpp

namespace
{
    constexpr size_t kRegisters = 13;

    struct Frame
    {
        u64 registers[kRegisters];
        std::vector<u8> stream;
    };

    LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        return DefWindowProcW(window, message, w, l);
    }

    std::vector<u8> readFile(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::vector<u8>(std::istreambuf_iterator<char>(file), {});
    }

}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: pcsx2_gs_replay <bin/resources> <capture> [frames] [key=value,...]\n");
        return 2;
    }
    const std::vector<u8> capture = readFile(argv[2]);
    const std::vector<u8> memory = readFile(std::string(argv[2]) + ".vram");
    if (capture.size() < 4 || std::memcmp(capture.data(), "OGIF", 4) != 0 || memory.size() != 4u * 1024u * 1024u)
    {
        std::fprintf(stderr, "bad capture\n");
        return 1;
    }
    std::vector<Frame> frames;
    for (size_t at = 4; at < capture.size();)
    {
        Frame frame;
        std::memcpy(frame.registers, capture.data() + at, sizeof(frame.registers));
        at += sizeof(frame.registers);
        u32 size;
        std::memcpy(&size, capture.data() + at, sizeof(size));
        at += sizeof(size);
        frame.stream.assign(capture.begin() + at, capture.begin() + at + size);
        at += size;
        frames.push_back(std::move(frame));
    }

    const int count = argc > 3 ? std::atoi(argv[3]) : 300;
    Pcsx2Config::GSOptions options;
    EmuFolders::Resources = argv[1];
    options.Renderer = GSRendererType::DX11;
    options.UpscaleMultiplier = 8.0f;
    options.AspectRatio = AspectRatioType::R16_9;
    options.InterlaceMode = GSInterlaceMode::Off;
    options.MaxAnisotropy = 16;
    options.AccurateBlendingUnit = AccBlendLevel::Medium;
    options.LinearPresent = GSPostBilinearMode::BilinearSharp;
    int width = 1777, height = 1000;
    if (argc > 4)
    {
        const std::string spec = argv[4];
        for (size_t at = 0; at < spec.size();)
        {
            const size_t end = std::min(spec.find(',', at), spec.size());
            const std::string item = spec.substr(at, end - at);
            at = end + 1;
            const size_t equals = item.find('=');
            const std::string key = item.substr(0, equals);
            const int value = std::atoi(item.c_str() + equals + 1);
            if (key == "scale")
                options.UpscaleMultiplier = static_cast<float>(value);
            else if (key == "fxaa")
                options.FXAA = value != 0;
            else if (key == "aniso")
                options.MaxAnisotropy = static_cast<u8>(value);
            else if (key == "blend")
                options.AccurateBlendingUnit = static_cast<AccBlendLevel>(value);
            else if (key == "present")
                options.LinearPresent = static_cast<GSPostBilinearMode>(value);
            else if (key == "mipmap")
                options.HWMipmap = value != 0;
            else if (key == "edge")
                g_orochi_sprite_edge_inset = value != 0;
            else if (key == "filter")
                options.TextureFiltering = static_cast<BiFiltering>(value);
            else if (key == "hpo")
            {
                options.ManualUserHacks = true;
                options.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(value);
            }
            else if (key == "round")
            {
                options.ManualUserHacks = true;
                options.UserHacks_RoundSprite = static_cast<s8>(value);
            }
            else if (key == "native")
            {
                options.ManualUserHacks = true;
                options.UserHacks_NativeScaling = static_cast<GSNativeScaling>(value);
            }
            else if (key == "align")
            {
                options.ManualUserHacks = true;
                options.UserHacks_AlignSpriteX = value != 0;
            }
            else if (key == "merge")
            {
                options.ManualUserHacks = true;
                options.UserHacks_MergePPSprite = value != 0;
            }
            else if (key == "tcx" || key == "tcy")
            {
                options.ManualUserHacks = true;
                (key == "tcx" ? options.UserHacks_TCOffsetX : options.UserHacks_TCOffsetY) = value;
            }
            else if (key == "w")
                width = value;
            else if (key == "h")
                height = value;
        }
    }

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"OrochiPcsx2GsReplay";
    RegisterClassW(&windowClass);
    // Off the desktop and never activated: it can run beside the game.
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"GS replay", WS_POPUP,
                                  -8000, 0, width, height, nullptr, nullptr, windowClass.hInstance, nullptr);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    OrochiPcsx2Host::SetRenderWindow(window, width, height);

    alignas(16) static u8 privileged[0x2000]{};
    EmuConfig.GS.AspectRatio = options.AspectRatio;
    EmuConfig.CurrentAspectRatio = options.AspectRatio;
    if (!GSopen(options, GSRendererType::DX11, privileged, GSVSyncMode::Disabled, false))
    {
        std::fprintf(stderr, "GSopen failed\n");
        return 1;
    }
    std::memcpy(g_gs_renderer->m_mem.vm8(), memory.data(), memory.size());

    // Each frame waits for the GPU, so the time includes the work it was given.
    GSDevice11 *device = GSDevice11::GetInstance();
    ID3D11Query *finished = nullptr;
    D3D11_QUERY_DESC query{};
    query.Query = D3D11_QUERY_EVENT;
    device->GetD3DDevice()->CreateQuery(&query, &finished);

    constexpr int kWarmup = 30;
    auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < count + kWarmup; ++frame)
    {
        if (frame == kWarmup)
        {
            start = std::chrono::steady_clock::now();
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&message);
        const Frame &current = frames[frame % frames.size()];
        const size_t offsets[] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xE0, 0x1000};
        for (size_t index = 0; index < kRegisters; ++index)
            std::memcpy(privileged + offsets[index], &current.registers[index], sizeof(u64));
        GSgifTransfer(current.stream.data(), static_cast<u32>(current.stream.size() / 16u));
        GSvsync(frame & 1, true);
        device->GetD3DContext()->End(finished);
        BOOL done = FALSE;
        while (device->GetD3DContext()->GetData(finished, &done, sizeof(done), 0) != S_OK || !done)
            ;
    }
    const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g_perfmon.Update();
    std::printf("%-30s %7.2f ms/frame (%d frames), draws %.0f, calls %.0f, copies %.0f, barriers %.0f, passes %.0f\n",
                argc > 4 ? argv[4] : "default", wall / count, count, g_perfmon.Get(GSPerfMon::Draw),
                g_perfmon.Get(GSPerfMon::DrawCalls), g_perfmon.Get(GSPerfMon::TextureCopies),
                g_perfmon.Get(GSPerfMon::Barriers), g_perfmon.Get(GSPerfMon::RenderPasses));
    // OROCHI_REPLAY_PPM=<file>: the last frame as a PPM, to compare options on the same frame.
    if (const char *ppm = std::getenv("OROCHI_REPLAY_PPM"))
    {
        u32 shotWidth = 0, shotHeight = 0;
        std::vector<u32> pixels;
        if (GSSaveSnapshotToMemory(static_cast<u32>(width), static_cast<u32>(height), true, false, &shotWidth, &shotHeight, &pixels))
        {
            std::ofstream out(ppm, std::ios::binary);
            out << "P6\n" << shotWidth << ' ' << shotHeight << "\n255\n";
            for (const u32 pixel : pixels)
            {
                const char rgb[3] = {static_cast<char>(pixel & 0xFF), static_cast<char>((pixel >> 8) & 0xFF),
                                     static_cast<char>((pixel >> 16) & 0xFF)};
                out.write(rgb, 3);
            }
            std::printf("saved %ux%u to %s\n", shotWidth, shotHeight, ppm);
        }
        else
            std::fprintf(stderr, "snapshot failed\n");
    }
    GSclose();
    return 0;
}
