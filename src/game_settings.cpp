// Turns the saved settings into what the game reads (src/game_settings.h).
#include "game_settings.h"

#include "game_patches.h"
#include "orochi_settings.h"
#include "raylib.h"

#include <algorithm>
#include <cstdlib>
#include <string>

bool orochiSetupPlacedWindow = false;

namespace
{
    // A variable set by hand (a launcher, the command line) wins over the file.
    void setDefault(const char *name, const std::string &value)
    {
        if (!std::getenv(name))
            _putenv_s(name, value.c_str());
    }
}

void showOrochiWindow()
{
    orochiRestoreWindow(GetWindowHandle());
}

void applyOrochiSettings(const OrochiSettings &settings)
{
    setDefault("OROCHI_RENDERER", settings.pcsx2 ? "pcsx2" : "auto");
    setDefault("OROCHI_PCSX2_SCALE", std::to_string(settings.scale));
    setDefault("OROCHI_SPU2", settings.audio ? "1" : "0");
    setDefault("OROCHI_VOLUME", std::to_string(settings.volume));
    setDefault("OROCHI_CROWD", std::to_string(settings.crowd));
    setDefault("OROCHI_AUTOSAVE", settings.autoSave ? "1" : "0");
    {
        std::string cheats;
        auto add = [&cheats](bool on, const char *name) {
            if (on)
                cheats += (cheats.empty() ? "" : ",") + std::string(name);
        };
        add(settings.cheatInvincible, "invincible");
        add(settings.cheatLife, "life");
        add(settings.cheatMusou, "musou");
        add(settings.cheatPoints, "points");
        setDefault("OROCHI_CHEATS", cheats);
    }
    setDefault("OROCHI_PCSX2_FIXES", std::string("deinterlace=") + (settings.deinterlace ? "0" : "1") +
                                         ",fxaa=" + (settings.fxaa ? "1" : "0") +
                                         (settings.seamFix ? ",edge=1,hpo=5" : ",edge=0"));
    if (!std::getenv("OROCHI_WIDESCREEN"))
        OrochiGamePatches::widescreen = settings.wide();
    // The whole window has to fit, title bar and taskbar included: Windows
    // would cut a taller one short and it would stop being 16:9.
    constexpr int kTitleBar = 32, kTaskbar = 48;
    const int monitor = GetCurrentMonitor();
    const int roomWidth = GetMonitorWidth(monitor), roomHeight = GetMonitorHeight(monitor) - kTitleBar - kTaskbar;
    const OrochiWindowSize size = settings.sizes()[settings.windowSize];
    const float fit = std::min({1.0f, static_cast<float>(roomWidth) / size.width,
                                static_cast<float>(roomHeight) / size.height});
    const int width = static_cast<int>(size.width * fit), height = static_cast<int>(size.height * fit);
    SetWindowSize(width, height);
    SetWindowPosition((roomWidth - width) / 2, kTitleBar + (roomHeight - height) / 2);
    // Full screen goes on top of the placed window, so F10 has a window to return to.
    if (settings.fullscreen && !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
        ToggleBorderlessWindowed();
    orochiSetupPlacedWindow = true;
}
