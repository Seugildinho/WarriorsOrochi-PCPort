#pragma once
// PCSX2's GS as the game's renderer (OROCHI_RENDERER=pcsx2).
// Arquivos envolvidos:
//   pcsx2_runtime.cpp   -> liga o GS do PCSX2 ao runtime (este cabecalho)
//   pcsx2_window.cpp    -> janela filha, F9 e F12
//   pcsx2_gs_bridge.cpp -> thread que executa o GS do PCSX2
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
    // A janela mudou de tamanho (pixels); repassa ao GS.
    void Resized(unsigned width, unsigned height);
    // F9 in the game window; true when a capture was requested.
    bool CaptureKey();
    // Pasta do WarriorsOrochi.exe, sem barra no final.
    std::string ExecutableDirectory();
}
