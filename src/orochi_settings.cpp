// The settings file (src/orochi_settings.h).
#include "orochi_settings.h"

#include "raylib.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace
{
    const std::vector<OrochiWindowSize> kWideSizes{{960, 540}, {1280, 720}, {1600, 900}, {1920, 1080}};
    const std::vector<OrochiWindowSize> kStandardSizes{{640, 480}, {960, 720}, {1280, 960}, {1440, 1080}};
    const std::vector<int> kScales{1, 2, 3, 4, 5, 6, 8};

    std::filesystem::path settingsPath() { return orochiProgramFolder() / "orochi-settings.ini"; }
}

const std::vector<OrochiWindowSize> &OrochiSettings::sizes() const
{
    return wide() ? kWideSizes : kStandardSizes;
}

const std::vector<int> &orochiScales()
{
    return kScales;
}

std::filesystem::path orochiProgramFolder()
{
    return orochiPathFromUtf8(GetApplicationDirectory());
}

std::string orochiUtf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path orochiPathFromUtf8(const std::string &text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

OrochiSettings orochiLoadSettings()
{
    OrochiSettings settings;
    std::ifstream file(settingsPath());
    std::string line, window;
    while (std::getline(file, line))
    {
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        if (key == "renderer")
            settings.pcsx2 = value == "pcsx2";
        else if (key == "scale")
            settings.scale = std::atoi(value.c_str());
        else if (key == "aspect")
            settings.widescreen = value == "16:9";
        else if (key == "display")
            settings.fullscreen = value == "fullscreen";
        else if (key == "window")
            window = value;
        else if (key == "field_blend") // not "deinterlace": that older key defaulted on
            settings.deinterlace = value == "auto";
        else if (key == "seam_fix")
            settings.seamFix = value != "off";
        else if (key == "fxaa")
            settings.fxaa = value == "1";
        else if (key == "audio")
            settings.audio = value != "off";
        else if (key == "crowd")
            settings.crowd = std::clamp(std::atoi(value.c_str()), 51, 147);
        else if (key == "volume")
            settings.volume = std::clamp(std::atoi(value.c_str()) / 10 * 10, 0, 100);
        else if (key == "autosave")
            settings.autoSave = value != "off";
        else if (key == "cheat_invincible")
            settings.cheatInvincible = value == "on";
        else if (key == "cheat_life")
            settings.cheatLife = value == "on";
        else if (key == "cheat_musou")
            settings.cheatMusou = value == "on";
        else if (key == "cheat_points")
            settings.cheatPoints = value == "on";
        else if (key == "disc" && orochiPathFromUtf8(value).is_absolute())
            settings.disc = value;
    }
    settings.pcsx2 = settings.pcsx2 && kOrochiHasPcsx2;
    if (std::find(kScales.begin(), kScales.end(), settings.scale) == kScales.end())
        settings.scale = 2;
    const auto &sizes = settings.sizes();
    for (size_t index = 0; index < sizes.size(); ++index)
        if (window == std::to_string(sizes[index].width) + "x" + std::to_string(sizes[index].height))
            settings.windowSize = static_cast<int>(index);
    return settings;
}

void orochiSaveSettings(const OrochiSettings &settings)
{
    const OrochiWindowSize size = settings.sizes()[settings.windowSize];
    std::ofstream file(settingsPath());
    file << "renderer=" << (settings.pcsx2 ? "pcsx2" : "own") << '\n'
         << "scale=" << settings.scale << '\n'
         << "aspect=" << (settings.widescreen ? "16:9" : "4:3") << '\n'
         << "display=" << (settings.fullscreen ? "fullscreen" : "window") << '\n'
         << "window=" << size.width << 'x' << size.height << '\n'
         << "field_blend=" << (settings.deinterlace ? "auto" : "off") << '\n'
         << "fxaa=" << (settings.fxaa ? 1 : 0) << '\n'
         << "seam_fix=" << (settings.seamFix ? "on" : "off") << '\n'
         << "audio=" << (settings.audio ? "on" : "off") << '\n'
         << "volume=" << settings.volume << '\n'
         << "crowd=" << settings.crowd << '\n'
         << "autosave=" << (settings.autoSave ? "on" : "off") << '\n'
         << "cheat_invincible=" << (settings.cheatInvincible ? "on" : "off") << '\n'
         << "cheat_life=" << (settings.cheatLife ? "on" : "off") << '\n'
         << "cheat_musou=" << (settings.cheatMusou ? "on" : "off") << '\n'
         << "cheat_points=" << (settings.cheatPoints ? "on" : "off") << '\n'
         << "disc=" << settings.disc << '\n';
}
