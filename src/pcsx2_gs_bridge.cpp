// Runs PCSX2's GS for the port: GIF packets and VBlanks from the runtime's GS
// frontend (its packet and VBlank taps) are queued to a thread that owns the
// GS and its Direct3D 11 device. Compiled into pcsx2_gs.
#include "pcsx2_host.h"

#include "Config.h"
#include "common/Console.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern bool g_orochi_sprite_edge_inset; // GSRendererHW.cpp (sprite edge correction)

namespace
{
    struct Command
    {
        enum class Kind
        {
            Packet,
            VSync,
            Resize,
            DumpMemory,
            Stop,
        } kind = Kind::Packet;
        std::vector<u8> data;
        OrochiPcsx2Gs::Registers registers{};
        bool odd = false;
        unsigned width = 0, height = 0;
    };

    std::mutex s_mutex;
    std::condition_variable s_wake, s_progress;
    std::deque<Command> s_queue;
    std::vector<u8> s_partial;  // bytes of an incomplete quadword
    unsigned s_vsyncsQueued = 0;
    bool s_ready = false, s_open = false;
    std::thread s_thread;
    alignas(16) u8 s_privileged[0x2000];

    // Frame capture for diagnostics (OROCHI_GIF_CAPTURE=<file>): once
    // <file>.trigger exists or F9 is pressed, the GIF bytes of the next
    // VBlank intervals (OROCHI_GIF_CAPTURE_VSYNCS, default 2) are written to <file> ("OGIF", then per
    // VBlank its registers and the size and bytes of what came before it), and
    // the GS memory as it stands to <file>.vram.
    const unsigned kCaptureVSyncs = std::getenv("OROCHI_GIF_CAPTURE_VSYNCS")
                                        ? static_cast<unsigned>(std::atoi(std::getenv("OROCHI_GIF_CAPTURE_VSYNCS")))
                                        : 2u;
    const char *const s_capturePath = std::getenv("OROCHI_GIF_CAPTURE");
    unsigned s_captureLeft = 0;
    std::atomic<bool> s_captureRequested{false};
    std::vector<u8> s_capture, s_captureFile;

    void captureVSync(const OrochiPcsx2Gs::Registers &registers)
    {
        if (!s_capturePath)
            return;
        if (s_captureLeft > 0)
        {
            const u32 size = static_cast<u32>(s_capture.size());
            const u8 *fields = reinterpret_cast<const u8 *>(&registers);
            s_captureFile.insert(s_captureFile.end(), fields, fields + sizeof(registers));
            s_captureFile.insert(s_captureFile.end(), reinterpret_cast<const u8 *>(&size),
                                 reinterpret_cast<const u8 *>(&size) + sizeof(size));
            s_captureFile.insert(s_captureFile.end(), s_capture.begin(), s_capture.end());
            s_capture.clear();
            if (--s_captureLeft == 0)
            {
                if (std::FILE *file = std::fopen(s_capturePath, "wb"))
                {
                    std::fwrite(s_captureFile.data(), 1, s_captureFile.size(), file);
                    std::fclose(file);
                    std::fprintf(stderr, "[gif-capture] %zu bytes to %s\n", s_captureFile.size(), s_capturePath);
                }
                s_captureFile.clear();
                Command dump;
                dump.kind = Command::Kind::DumpMemory;
                s_queue.push_back(std::move(dump));
            }
            return;
        }
        const std::string trigger = std::string(s_capturePath) + ".trigger";
        if (s_captureRequested.exchange(false) || std::remove(trigger.c_str()) == 0)
        {
            s_captureLeft = kCaptureVSyncs;
            s_captureFile.assign({'O', 'G', 'I', 'F'});
        }
    }

    void dumpMemory()
    {
        const std::string path = std::string(s_capturePath) + ".vram";
        if (std::FILE *file = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(g_gs_renderer->m_mem.vm8(), 1, 4u * 1024u * 1024u, file);
            std::fclose(file);
        }
    }

    void writeRegisters(const OrochiPcsx2Gs::Registers &r, bool odd)
    {
        auto put = [](u32 offset, u64 value) { std::memcpy(s_privileged + offset, &value, sizeof(value)); };
        put(0x00, r.pmode);
        put(0x10, r.smode1);
        put(0x20, r.smode2);
        put(0x30, r.srfsh);
        put(0x40, r.synch1);
        put(0x50, r.synch2);
        put(0x60, r.syncv);
        put(0x70, r.dispfb1);
        put(0x80, r.display1);
        put(0x90, r.dispfb2);
        put(0xA0, r.display2);
        put(0xE0, r.bgcolor);
        // CSR.FIELD (bit 13): the field being displayed.
        put(0x1000, (r.csr & ~(1ull << 13)) | (odd ? 1ull << 13 : 0ull));
    }

    // PCSX2's upscaling corrections from "key=value,..." (OROCHI_PCSX2_FIXES):
    // hpo (GSHalfPixelOffset), round (round sprite 0..2), align, merge, even
    // (align/merge/force-even sprites 0/1), native (GSNativeScaling), tcx, tcy
    // (texture offsets), deinterlace (GSInterlaceMode), fxaa (0/1), scale,
    // aniso (max anisotropy), blend (AccBlendLevel), trilinear (TriFiltering),
    // present (GSPostBilinearMode), cas (0/1, sharpening), sharpness (0..100),
    // preload (TexturePreloadingLevel), mipmap (0/1).
    void applyFixes(Pcsx2Config::GSOptions &options, const std::string &spec)
    {
        size_t at = 0;
        while (at < spec.size())
        {
            const size_t end = std::min(spec.find(',', at), spec.size());
            const std::string item = spec.substr(at, end - at);
            at = end + 1;
            const size_t equals = item.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = item.substr(0, equals);
            const int value = std::atoi(item.c_str() + equals + 1);
            options.ManualUserHacks = true;
            if (key == "hpo")
                options.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(
                    std::clamp(value, 0, static_cast<int>(GSHalfPixelOffset::MaxCount) - 1));
            else if (key == "round")
                options.UserHacks_RoundSprite = static_cast<s8>(std::clamp(value, 0, 2));
            else if (key == "align")
                options.UserHacks_AlignSpriteX = value != 0;
            else if (key == "merge")
                options.UserHacks_MergePPSprite = value != 0;
            else if (key == "even")
                options.UserHacks_ForceEvenSpritePosition = value != 0;
            else if (key == "native")
                options.UserHacks_NativeScaling = static_cast<GSNativeScaling>(std::clamp(value, 0, 4));
            else if (key == "tcx")
                options.UserHacks_TCOffsetX = value;
            else if (key == "tcy")
                options.UserHacks_TCOffsetY = value;
            else if (key == "deinterlace")
                options.InterlaceMode = static_cast<GSInterlaceMode>(
                    std::clamp(value, 0, static_cast<int>(GSInterlaceMode::Count) - 1));
            else if (key == "fxaa")
                options.FXAA = value != 0;
            else if (key == "scale")
                options.UpscaleMultiplier = static_cast<float>(std::clamp(value, 1, 8));
            else if (key == "aniso")
                options.MaxAnisotropy = static_cast<u8>(std::clamp(value, 0, 16));
            else if (key == "blend")
                options.AccurateBlendingUnit = static_cast<AccBlendLevel>(
                    std::clamp(value, 0, static_cast<int>(AccBlendLevel::MaxCount) - 1));
            else if (key == "trilinear")
                options.TriFilter = static_cast<TriFiltering>(std::clamp(value, -1, 2));
            else if (key == "present")
                options.LinearPresent = static_cast<GSPostBilinearMode>(std::clamp(value, 0, 2));
            else if (key == "cas")
                options.CASMode = value != 0 ? GSCASMode::SharpenOnly : GSCASMode::Disabled;
            else if (key == "sharpness")
                options.CAS_Sharpness = static_cast<u8>(std::clamp(value, 0, 100));
            else if (key == "preload")
                options.TexturePreloading = static_cast<TexturePreloadingLevel>(std::clamp(value, 0, 2));
            else if (key == "mipmap")
                options.HWMipmap = value != 0;
            else if (key == "edge")
                g_orochi_sprite_edge_inset = value != 0;
        }
    }

    void run(std::string resources, unsigned upscale, bool widescreen)
    {
        // PCSX2's log (device and shader errors) on stderr.
        Log::SetHostOutputLevel(LOGLEVEL_INFO, [](LOGLEVEL, ConsoleColors, std::string_view message) {
            std::fprintf(stderr, "[pcsx2] %.*s\n", static_cast<int>(message.size()), message.data());
        });
        EmuFolders::Resources = std::move(resources);
        // Compiled shaders are kept beside the saves; without a folder each
        // one is compiled again the first time a run draws with it.
        std::error_code error;
        const auto cache = std::filesystem::current_path(error) / "shader-cache";
        std::filesystem::create_directories(cache, error);
        if (!error)
            EmuFolders::Cache = cache.string();
        Pcsx2Config::GSOptions options;
        options.Renderer = GSRendererType::DX11;
        options.UpscaleMultiplier = static_cast<float>(upscale);
        options.AspectRatio = widescreen ? AspectRatioType::R16_9 : AspectRatioType::R4_3;
        // The port's picture: the field offset is patched out of the game
        // (fixedFieldOffset), so nothing needs PCSX2's field blending, and the
        // defaults below sharpen textures at a distance and the final scaling.
        options.InterlaceMode = GSInterlaceMode::Off;
        options.MaxAnisotropy = 16;
        options.AccurateBlendingUnit = AccBlendLevel::Medium;
        options.LinearPresent = GSPostBilinearMode::BilinearSharp;
        if (const char *fixes = std::getenv("OROCHI_PCSX2_FIXES"))
            applyFixes(options, fixes);
        // The display reads the current ratio (PCSX2's VM sets it per FMV).
        EmuConfig.GS.AspectRatio = options.AspectRatio;
        EmuConfig.CurrentAspectRatio = options.AspectRatio;
        const bool open = GSopen(options, GSRendererType::DX11, s_privileged, GSVSyncMode::Disabled, false);
        {
            std::lock_guard lock(s_mutex);
            s_ready = true;
            s_open = open;
        }
        s_progress.notify_all();
        if (!open)
            return;
        std::deque<Command> batch;
        for (;;)
        {
            {
                std::unique_lock lock(s_mutex);
                s_wake.wait(lock, [] { return !s_queue.empty(); });
                batch.swap(s_queue);
            }
            for (Command &command : batch)
            {
                switch (command.kind)
                {
                case Command::Kind::Packet:
                    GSgifTransfer(command.data.data(), static_cast<u32>(command.data.size() / 16u));
                    break;
                case Command::Kind::VSync:
                    writeRegisters(command.registers, command.odd);
                    GSvsync(command.odd ? 1u : 0u, true);
                    {
                        std::lock_guard lock(s_mutex);
                        --s_vsyncsQueued;
                    }
                    s_progress.notify_all();
                    break;
                case Command::Kind::Resize:
                    OrochiPcsx2Host::SetRenderWindow(nullptr, command.width, command.height);
                    GSResizeDisplayWindow(command.width, command.height, 1.0f);
                    break;
                case Command::Kind::DumpMemory:
                    dumpMemory();
                    break;
                case Command::Kind::Stop:
                    GSclose();
                    return;
                }
            }
            batch.clear();
        }
    }

    void push(Command command)
    {
        {
            std::lock_guard lock(s_mutex);
            s_queue.push_back(std::move(command));
        }
        s_wake.notify_one();
    }
}

bool OrochiPcsx2Gs::Start(const char *resources, void *hwnd, unsigned width, unsigned height, unsigned upscale,
                          bool widescreen)
{
    OrochiPcsx2Host::SetRenderWindow(hwnd, width, height);
    s_thread = std::thread(run, std::string(resources), upscale, widescreen);
    std::unique_lock lock(s_mutex);
    s_progress.wait(lock, [] { return s_ready; });
    if (!s_open)
    {
        lock.unlock();
        s_thread.join();
        return false;
    }
    return true;
}

void OrochiPcsx2Gs::Packet(const uint8_t *data, uint32_t size)
{
    Command command;
    {
        std::lock_guard lock(s_mutex);
        if (!s_open)
            return;
        if (s_captureLeft > 0)
            s_capture.insert(s_capture.end(), data, data + size);
        s_partial.insert(s_partial.end(), data, data + size);
        const size_t whole = s_partial.size() & ~size_t(15);
        if (whole == 0u)
            return;
        command.data.assign(s_partial.begin(), s_partial.begin() + static_cast<std::ptrdiff_t>(whole));
        s_partial.erase(s_partial.begin(), s_partial.begin() + static_cast<std::ptrdiff_t>(whole));
        s_queue.push_back(std::move(command));
    }
    s_wake.notify_one();
}

void OrochiPcsx2Gs::VSync(const Registers &registers, bool oddField)
{
    {
        std::unique_lock lock(s_mutex);
        if (!s_open)
            return;
        s_progress.wait(lock, [] { return s_vsyncsQueued < 2u; });
        captureVSync(registers);
        Command command;
        command.kind = Command::Kind::VSync;
        command.registers = registers;
        command.odd = oddField;
        s_queue.push_back(std::move(command));
        ++s_vsyncsQueued;
    }
    s_wake.notify_one();
}

void OrochiPcsx2Gs::Resize(unsigned width, unsigned height)
{
    Command command;
    command.kind = Command::Kind::Resize;
    command.width = width;
    command.height = height;
    push(std::move(command));
}

bool OrochiPcsx2Gs::RequestCapture()
{
    if (!s_capturePath)
        return false;
    s_captureRequested = true;
    return true;
}

void OrochiPcsx2Gs::Stop()
{
    if (!s_thread.joinable())
        return;
    Command command;
    command.kind = Command::Kind::Stop;
    push(std::move(command));
    s_thread.join();
    std::lock_guard lock(s_mutex);
    s_open = false;
}
