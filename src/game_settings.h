#pragma once
struct OrochiSettings;

// Applies saved settings to the game: renderer, PCSX2 internal scale, 16:9,
// deinterlacing, FXAA, audio, autosave, window size or borderless fullscreen.
// They become the environment variables and flags the port already reads, and
// a variable set by hand wins. Needs the runtime's window.
void applyOrochiSettings(const OrochiSettings &settings);

// The window can open minimized, where the game stands still.
void showOrochiWindow();

// True once the settings placed the window (their size, centred on the monitor).
extern bool orochiSetupPlacedWindow;
