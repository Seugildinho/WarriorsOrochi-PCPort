// Wires PCSX2's GS (src/pcsx2_gs_bridge.cpp) to the runtime: the GS frontend's
// packet and VBlank taps go to it, and the frontend's raster backend becomes
// one that draws nothing.
// ==========================================================================
// LIGA O GS DO PCSX2 AO RUNTIME DO JOGO
// ==========================================================================
// O runtime (vendor/PS2Recomp/ps2xRuntime) tem o seu proprio "GS frontend":
// ele recebe os pacotes GIF do jogo e entende registradores de controle
// (CSR, SIGNAL, FINISH) que o jogo le de volta. Este arquivo:
//   1. Mantem esse frontend funcionando (o jogo depende dessas respostas),
//      mas troca o desenho dele por um que NAO desenha nada
//      (NullRasterBackend), para nao gastar CPU desenhando duas vezes.
//   2. Instala "torneiras" (taps): cada pacote GIF e cada VBlank que o
//      frontend recebe tambem e enviado ao GS do PCSX2, que e quem desenha
//      de verdade na tela (pcsx2_gs_bridge.cpp).
//   3. Define a resolucao interna (OROCHI_PCSX2_SCALE) e o formato 16:9.
#include "pcsx2_runtime.h"

#include "game_patches.h"
#include "pcsx2_host.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "raylib.h"
#include "game_settings.h"

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace
{
    // NullRasterBackend: substituto "mudo" do desenhista do runtime.
    // Todas as funcoes aceitam o comando e nao fazem nada, porque quem desenha
    // e o PCSX2. Limitacao conhecida: leituras da VRAM pelo jogo (GS -> EE,
    // ConsumeLocalToHostBytes/ReadVram) devolvem zero; ainda nao estao ligadas
    // a memoria do PCSX2. Se um efeito depender disso, ele aparecera errado e o
    // log mostrara "[Orochi:pcsx2-gs] local-to-host transfer not supported".
    class NullRasterBackend final : public GSRasterBackend
    {
    public:
        void Initialize(uint8_t *, uint32_t) override {}
        void Reset() override {}
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void BeginTransfer(const GSTransferCommand &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void Flush() override {}
        void TextureFlush() override {}
        void Sync(GSSyncReason) override {}
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return true; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override
        {
            // GS-to-EE transfers would need PCSX2's local memory (not wired yet).
            static bool reported = false;
            if (!reported)
            {
                reported = true;
                std::cerr << "[Orochi:pcsx2-gs] local-to-host transfer not supported\n";
            }
            return 0u;
        }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0u; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &out) const override { out.clear(); }
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }
    };
}

// Resized: chamada pela janela (pcsx2_window.cpp) quando o tamanho muda;
// repassa ao GS para ele redimensionar a imagem.
void OrochiPcsx2Window::Resized(unsigned width, unsigned height)
{
    OrochiPcsx2Gs::Resize(width, height);
}

// CaptureKey: tecla F9 -> pede captura de diagnostico de quadros.
bool OrochiPcsx2Window::CaptureKey()
{
    return OrochiPcsx2Gs::RequestCapture();
}

// installOrochiPcsx2Gs: escolhe e liga o PCSX2 como renderizador.
// Chamada uma vez em main.cpp, antes do jogo comecar. Passos:
//   1. Se o patch widescreen esta ativo e o Setup nao posicionou a janela,
//      usa uma janela 960x540 (16:9).
//   2. Cria a janela filha onde o Direct3D 11 desenha.
//   3. Resolucao interna: padrao 2x; OROCHI_PCSX2_SCALE muda.
//      Exemplo: OROCHI_PCSX2_SCALE=4 -> 2560x1792 internos (4 x 640x448).
//      O Setup grava esse valor a partir da aba Graficos.
//   4. Inicia o GS; se falhar retorna false e main.cpp usa outro renderer.
//   5. Troca o desenhista do runtime pelo NullRasterBackend e instala as
//      taps de pacote e de VBlank.
bool installOrochiPcsx2Gs(PS2Runtime &runtime)
{
    const bool widescreen = OrochiGamePatches::widescreen;
    // A 16:9 window for the 16:9 picture, unless the setup screen placed it.
    if (widescreen && !orochiSetupPlacedWindow)
        SetWindowSize(960, 540);
    unsigned width = 0, height = 0;
    void *window = OrochiPcsx2Window::Create(GetWindowHandle(), width, height);
    if (!window)
        return false;
    unsigned upscale = 2;
    if (const char *scale = std::getenv("OROCHI_PCSX2_SCALE"))
        upscale = static_cast<unsigned>(std::max(1, std::atoi(scale)));
    const std::string resources = OrochiPcsx2Window::ExecutableDirectory() + "/pcsx2-resources";
    if (!OrochiPcsx2Gs::Start(resources.c_str(), window, width, height, upscale, widescreen))
        return false;
    runtime.gs().setRasterBackend(std::make_unique<NullRasterBackend>());
    runtime.gs().setPacketTap([](const uint8_t *data, uint32_t size) { OrochiPcsx2Gs::Packet(data, size); });
    // Tap de VBlank: roda uma vez por quadro (cerca de 60 vezes por segundo).
    // "tick" conta os VBlanks desde o inicio; par = campo par, impar = campo
    // impar (ultimo argumento de VSync()).
    runtime.gs().setVsyncTap([](const GSRegisters &r, uint64_t tick) {
        // Pauses the player would feel ("travadas"): a frame later than 100 ms.
        // Loading screens show here too; the log line carries the tick to tell.
        {
            using Clock = std::chrono::steady_clock;
            static Clock::time_point last{};
            static int reported = 0;
            const Clock::time_point now = Clock::now();
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            if (last != Clock::time_point{} && gap >= 100 && reported < 300)
            {
                ++reported;
                std::cout << "[Orochi:hitch] vsync=" << tick << " ms=" << gap << std::endl;
            }
            last = now;
        }
        // The BIOS's SetGsCrt writes SMODE1 and SYNCV, which the runtime leaves
        // at zero: PCSX2 reads CMOD 0 as VESA, and an even SYNCV.VFP as not
        // interlaced, so field-mode frames (SMODE2.FFMD, the pause menu) were
        // shown without their field offset and the gaps between fields showed
        // as dashed lines. These are the BIOS's NTSC interlaced values.
        const uint64_t smode1 = r.smode1 ? r.smode1 : 0x740834504ull;
        const uint64_t syncv = r.syncv ? r.syncv : 0xC7800601A01801ull;
        // Copia os registradores de video atuais para o formato da ponte e envia
        // o quadro. Pode BLOQUEAR aqui se o GS estiver 2 quadros atrasado (veja
        // OrochiPcsx2Gs::VSync): e isso que segura o jogo no ritmo da tela.
        const OrochiPcsx2Gs::Registers registers{r.pmode, smode1, r.smode2, r.srfsh, r.synch1, r.synch2, syncv,
                                                 r.dispfb1, r.display1, r.dispfb2, r.display2, r.bgcolor,
                                                 r.csr.load(std::memory_order_relaxed)};
        OrochiPcsx2Gs::VSync(registers, (tick & 1u) != 0u);
    });
    std::cout << "[Orochi] PCSX2 GS renderer (Direct3D 11), internal scale " << upscale << "x, window "
              << width << 'x' << height << (widescreen ? ", 16:9" : ", 4:3") << '\n';
    return true;
}
