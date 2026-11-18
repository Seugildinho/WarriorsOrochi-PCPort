#pragma once
// The settings shared by the two programs: WarriorsOrochi-Setup.exe writes
// them and WarriorsOrochi.exe starts the game with them. They live in
// orochi-settings.ini, next to both executables.
#include <filesystem>
#include <string>
#include <vector>

inline constexpr wchar_t kOrochiGameProgram[] = L"WarriorsOrochi.exe";
inline constexpr wchar_t kOrochiSetupProgram[] = L"WarriorsOrochi-Setup.exe";

#ifdef OROCHI_PCSX2_GS
inline constexpr bool kOrochiHasPcsx2 = true;
#else
inline constexpr bool kOrochiHasPcsx2 = false;
#endif

struct OrochiWindowSize
{
    int width, height;
};

struct OrochiSettings
{
    bool pcsx2 = kOrochiHasPcsx2;
    int scale = 2;
    bool widescreen = true;
    bool fullscreen = false;
    int windowSize = 1;       // index into the size list of the chosen aspect
    bool deinterlace = false; // PCSX2's automatic deinterlacing
    bool fxaa = false;
    // Upscaling line fixes: sprite edge inset (no texel bleed on HUD art) and
    // PCSX2's native half-pixel offset with texture offset (post-processing strips).
    bool seamFix = true;
    bool audio = true;
    int volume = 100; // percent, in steps of 10
    // Characters drawn in full (OROCHI_CROWD): 51 is the game's own maximum.
    int crowd = 100;
    bool autoSave = true;
    // Cheats (OROCHI_CHEATS): damage ignored; life, musou and growth points kept full.
    bool cheatInvincible = false, cheatLife = false, cheatMusou = false, cheatPoints = false;
    std::string disc; // full path, in UTF-8; empty until one is chosen

    bool wide() const { return pcsx2 && widescreen; }
    const std::vector<OrochiWindowSize> &sizes() const;
};

const std::vector<int> &orochiScales();
OrochiSettings orochiLoadSettings();
void orochiSaveSettings(const OrochiSettings &settings);

// The folder of both executables, where the ini and the data folder are.
std::filesystem::path orochiProgramFolder();
std::string orochiUtf8(const std::filesystem::path &path);
std::filesystem::path orochiPathFromUtf8(const std::string &text);

// Win32 side (src/setup_win32.cpp), kept apart from raylib's headers.
// Starts the other program, from the same folder.
bool orochiRunProgram(const wchar_t *name);
// The file dialog for choosing a disc image; false when cancelled.
bool orochiPickDisc(void *ownerWindow, std::filesystem::path &disc);
// Brings a window that opened minimized back: the game stands still while minimized.
void orochiRestoreWindow(void *window);
// Without a console or a launcher's redirection, keeps the output in `path`.
void orochiLogToFile(const std::filesystem::path &path);
