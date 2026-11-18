// The PCSX2 services its GS calls, for the GS running inside the port
// (relatorios/pcsx2-gs-integracao.md). The port has no PCSX2 VM, UI, capture
// or input: those entry points do nothing. Compiled into pcsx2_gs with the
// PCSX2 headers and definitions.
#include "pcsx2_host.h"

#include "common/Image.h"
#include "common/WindowInfo.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/Debug.h"
#include "GS/GSCapture.h"
#include "GS/GSDump.h"
#include "GS/GSPng.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "Host/AudioStream.h"
#include "ImGui/FullscreenUI.h"
#include "ImGui/ImGuiManager.h"
#include "Memory.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "R5900.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Pad/Pad.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_freetype.h"

#include "common/RedtapeWindows.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace
{
    std::mutex s_windowMutex;
    std::optional<WindowInfo> s_window;
}

void OrochiPcsx2Host::SetRenderWindow(void *hwnd, unsigned width, unsigned height)
{
    std::lock_guard lock(s_windowMutex);
    WindowInfo info;
    info.type = WindowInfo::Type::Win32;
    // A null handle keeps the current window (a resize).
    info.window_handle = hwnd ? hwnd : (s_window ? s_window->window_handle : nullptr);
    info.surface_width = width;
    info.surface_height = height;
    info.surface_scale = 1.0f;
    s_window = info;
}

// ---- EE state the GS headers refer to (never read: the port has no PCSX2 EE)
alignas(__pagealignsize) u8 eeHw[Ps2MemSize::Hardware];
alignas(16) cpuRegistersPack _cpuRegistersPack;
bool FMVstarted = false;

// ---- Executable memory for the software renderer's code generator
u8 *SysMemory::GetCodePtr(size_t offset)
{
    static u8 *base = [] {
        auto *memory = static_cast<u8 *>(VirtualAlloc(nullptr, HostMemoryMap::CodeSize, MEM_RESERVE, PAGE_NOACCESS));
        if (memory)
            VirtualAlloc(memory + HostMemoryMap::SWrecOffset, HostMemoryMap::SWrecSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        return memory;
    }();
    return base + offset;
}

// ---- Host
std::optional<WindowInfo> Host::AcquireRenderWindow(bool)
{
    std::lock_guard lock(s_windowMutex);
    return s_window;
}
void Host::ReleaseRenderWindow() {}
void Host::BeginPresentFrame() {}
bool Host::IsFullscreen() { return false; }
void Host::SetFullscreen(bool) {}
void Host::RunOnCPUThread(std::function<void()> function, bool) { function(); }
void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
    std::fprintf(stderr, "[pcsx2-gs] %.*s: %.*s\n", static_cast<int>(title.size()), title.data(),
                 static_cast<int>(message.size()), message.data());
}
void Host::AddKeyedOSDMessage(std::string, std::string message, float)
{
    std::fprintf(stderr, "[pcsx2-gs] %s\n", message.c_str());
}
void Host::AddIconOSDMessage(std::string, const char *, const std::string_view message, float)
{
    std::fprintf(stderr, "[pcsx2-gs] %.*s\n", static_cast<int>(message.size()), message.data());
}
std::string Host::GetStringSettingValue(const char *, const char *, const char *default_value)
{
    return default_value ? default_value : "";
}
bool Host::GetBaseBoolSettingValue(const char *, const char *, bool default_value) { return default_value; }
s32 Host::Internal::GetTranslatedStringImpl(const std::string_view, const std::string_view msg, char *tbuf, size_t tbuf_space)
{
    if (msg.size() > tbuf_space)
        return -1;
    std::memcpy(tbuf, msg.data(), msg.size());
    return static_cast<s32>(msg.size());
}
std::string Host::TranslateToString(const std::string_view, const std::string_view msg) { return std::string(msg); }
std::string_view Host::TranslateToStringView(const std::string_view, const std::string_view msg) { return msg; }
const char *Host::TranslateToCString(const std::string_view, const std::string_view msg)
{
    thread_local std::string text;
    text = msg;
    return text.c_str();
}

// ---- VM and GS thread: the port drives the GS from its own GS thread
bool VMManager::HasValidVM() { return true; }
std::string VMManager::GetDiscSerial() { return "SLUS-21662"; }
std::string VMManager::GetTitle(bool) { return "Warriors Orochi"; }
u32 VMManager::GetDiscCRC() { return 0; }
const std::vector<u32> &VMManager::Internal::GetSoftwareRendererProcessorList()
{
    static const std::vector<u32> none;
    return none;
}
void MTGS::RunOnGSThread(std::function<void()> function) { function(); }
void MTGS::WaitGS(bool, bool, bool) {}
void MTGS::ApplySettings() {}
void MTGS::ToggleSoftwareRendering() {}

const char *ReportVideoMode() { return "NTSC"; }
const char *ReportInterlaceMode() { return "Interlaced"; }
double GetVerticalFrequency() { return 59.94; }

PerformanceMetrics::InternalFPSMethod PerformanceMetrics::GetInternalFPSMethod() { return InternalFPSMethod::None; }
void PerformanceMetrics::Update(bool, bool, bool) {}
void PerformanceMetrics::OnGPUPresent(float, u64, u64) {}
void PerformanceMetrics::SetGSSWThread(u32, Threading::ThreadHandle) {}
void PerformanceMetrics::SetGSSWThreadCount(u32) {}

// ---- No on-screen UI
// The GS device renders ImGui's draw data on every present, so an (empty)
// context must exist; the port never adds windows to it.
bool ImGuiManager::Initialize()
{
    if (!ImGui::GetCurrentContext())
        ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    if (io.Fonts->Fonts.empty())
    {
        io.Fonts->SetFontLoader(ImFontAtlasGetFontLoaderForStbTruetype());
        io.Fonts->AddFontDefault();
    }
    WindowResized();
    ImGui::NewFrame();
    return true;
}
void ImGuiManager::Shutdown(bool)
{
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
}
void ImGuiManager::NewFrame()
{
    if (ImGui::GetCurrentContext())
        ImGui::NewFrame();
}
void ImGuiManager::SkipFrame()
{
    if (ImGui::GetCurrentContext())
    {
        ImGui::EndFrame();
        ImGui::NewFrame();
    }
}
void ImGuiManager::RenderOSD() {}
void ImGuiManager::ReloadFonts() {}
void ImGuiManager::WindowResized()
{
    if (!ImGui::GetCurrentContext())
        return;
    std::lock_guard lock(s_windowMutex);
    if (s_window)
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(s_window->surface_width), static_cast<float>(s_window->surface_height));
}
void ImGuiManager::RequestScaleUpdate() {}
void FullscreenUI::Render() {}
const ImFontLoader *ImGuiFreeType::GetFontLoader() { return nullptr; }
bool GSDumpReplayer::IsReplayingDump() { return false; }
bool GSDumpReplayer::IsRunner() { return false; }
void GSDumpReplayer::RenderUI() {}

// ---- No capture, dumps, PNG snapshots or texture replacement
bool GSCapture::BeginCapture(float, GSVector2i, float, std::string) { return false; }
bool GSCapture::DeliverVideoFrame(GSTexture *) { return false; }
void GSCapture::EndCapture() {}
void GSCapture::Flush() {}
bool GSCapture::IsCapturing() { return false; }
bool GSCapture::IsCapturingVideo() { return false; }
GSVector2i GSCapture::GetSize() { return GSVector2i(0, 0); }
std::string GSCapture::GetNextCaptureFileName() { return {}; }
bool GSPng::Save(GSPng::Format, const std::string &, const u8 *, int, int, int, int, bool) { return false; }
RGBA8Image::RGBA8Image() = default;
RGBA8Image::RGBA8Image(RGBA8Image &&move) : Image(std::move(move)) {}
bool RGBA8Image::SaveToFile(const char *, u8) const { return false; }
std::unique_ptr<GSDumpBase> GSDumpBase::CreateUncompressedDump(const std::string &, const std::string &, u32, u32, u32, const u32 *,
    const freezeData &, const GSPrivRegSet *)
{
    return {};
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateXzDump(const std::string &, const std::string &, u32, u32, u32, const u32 *,
    const freezeData &, const GSPrivRegSet *)
{
    return {};
}
std::unique_ptr<GSDumpBase> GSDumpBase::CreateZstDump(const std::string &, const std::string &, u32, u32, u32, const u32 *,
    const freezeData &, const GSPrivRegSet *)
{
    return {};
}
bool GSDumpBase::VSync(int, bool, const GSPrivRegSet *) { return false; }
void GSDumpBase::Transfer(int, const u8 *, size_t) {}
void GSDumpBase::ReadFIFO(u32) {}
void GSTextureReplacements::Initialize() {}
void GSTextureReplacements::GameChanged() {}
void GSTextureReplacements::ReloadReplacementMap() {}
void GSTextureReplacements::UpdateConfig(Pcsx2Config::GSOptions &) {}
void GSTextureReplacements::Shutdown() {}
bool GSTextureReplacements::HasAnyReplacementTextures() { return false; }
bool GSTextureReplacements::HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey &) { return false; }
GSTexture *GSTextureReplacements::LookupReplacementTexture(const GSTextureCache::HashCacheKey &, bool, bool *,
                                                           std::pair<u8, u8> *)
{
    return nullptr;
}
void GSTextureReplacements::ProcessAsyncLoadedTextures() {}
void GSTextureReplacements::DumpTexture(const GSTextureCache::HashCacheKey &, const GIFRegTEX0 &, const GIFRegTEXA &,
                                        GSTextureCache::SourceRegion, GSLocalMemory &, u32)
{
}

// ---- Settings code in Pcsx2Config.cpp refers to these subsystems
std::string Pad::GetConfigSection(u32 port) { return "Pad" + std::to_string(port + 1); }
Pad::ControllerType Pad::GetDefaultPadType(u32) { return Pad::ControllerType::NotConnected; }
const Pad::ControllerInfo *Pad::GetControllerInfo(Pad::ControllerType) { return nullptr; }
const Pad::ControllerInfo *Pad::GetControllerInfoByName(const std::string_view) { return nullptr; }
std::string USB::GetConfigSection(int port) { return "USB" + std::to_string(port + 1); }
const char *USB::DeviceTypeIndexToName(s32) { return "None"; }
s32 USB::DeviceTypeNameToIndex(const std::string_view) { return 0; }
bool FileMcd_IsMultitapSlot(uint) { return false; }
std::string FileMcd_GetDefaultName(uint slot) { return "Mcd00" + std::to_string(slot + 1) + ".ps2"; }
uint FileMcd_GetMtapPort(uint slot) { return slot; }
uint FileMcd_GetMtapSlot(uint) { return 0; }
const char *AudioStream::GetBackendName(AudioBackend) { return "Null"; }
std::optional<AudioBackend> AudioStream::ParseBackendName(const char *) { return std::nullopt; }
void AudioStreamParameters::LoadSave(SettingsWrapper &, const char *) {}
