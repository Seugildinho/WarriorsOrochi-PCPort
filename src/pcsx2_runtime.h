#pragma once
// PCSX2's GS as the game's renderer (OROCHI_RENDERER=pcsx2).
#include <string>

class PS2Runtime;

// Opens PCSX2's GS in a child of the game window and feeds it the runtime's GS
// input; the runtime's own GS keeps parsing packets (CSR, SIGNAL, FINISH) but
// draws nothing. False when the GS could not start.
bool installOrochiPcsx2Gs(PS2Runtime &runtime);

namespace OrochiPcsx2Window
{
    // A disabled child window covering `parent`'s client area; its size.
    void *Create(void *parent, unsigned &width, unsigned &height);
    void Resized(unsigned width, unsigned height);
    // F9 in the game window; true when a capture was requested.
    bool CaptureKey();
    std::string ExecutableDirectory();
}
