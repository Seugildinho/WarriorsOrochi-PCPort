// Turns the saved settings into what the game reads (src/game_settings.h).
// Fluxo das configuracoes:
//   WarriorsOrochi-Setup.exe grava orochi-settings.ini
//   -> WarriorsOrochi.exe le o ini (orochiLoadSettings)
//   -> applyOrochiSettings (aqui) transforma cada opcao numa variavel de
//      ambiente OROCHI_* que o resto do port le.
// Assim, todo ajuste tambem pode ser testado sem o Setup, definindo a
// variavel antes de abrir o jogo (ela tem prioridade sobre o ini).
#include "game_settings.h"

#include "game_patches.h"
#include "orochi_settings.h"
#include "raylib.h"

#include <algorithm>
#include <cstdlib>
#include <string>

// true depois que applyOrochiSettings posicionou a janela; o renderizador
// PCSX2 entao nao muda o tamanho dela (pcsx2_runtime.cpp).
bool orochiSetupPlacedWindow = false;

namespace
{
    // A variable set by hand (a launcher, the command line) wins over the file.
    // setDefault: define a variavel de ambiente SO se ela ainda nao existir.
    // Exemplo: se o usuario abriu com OROCHI_PCSX2_SCALE=6, o "scale=2" do ini
    // nao sobrescreve.
    void setDefault(const char *name, const std::string &value)
    {
        if (!std::getenv(name))
            _putenv_s(name, value.c_str());
    }
}

// showOrochiWindow: traz a janela de volta se ela abriu minimizada (o jogo
// fica parado enquanto minimizado).
void showOrochiWindow()
{
    orochiRestoreWindow(GetWindowHandle());
}

// applyOrochiSettings: aplica as opcoes do Setup. Correspondencia:
//   renderer/scale       -> OROCHI_RENDERER / OROCHI_PCSX2_SCALE (resolucao)
//   crowd                -> OROCHI_CROWD (soldados desenhados)
//   field_blend/fxaa/seam_fix -> OROCHI_PCSX2_FIXES (qualidade de imagem)
//   aspect               -> OrochiGamePatches::widescreen (16:9)
//   window/display       -> tamanho, posicao e tela cheia da janela
// (audio, autosave e trapacas tambem viram variaveis aqui.)
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
    // Monta OROCHI_PCSX2_FIXES (lida em pcsx2_gs_bridge.cpp, applyFixes):
    //   deinterlace=0 -> desentrelacamento automatico do PCSX2; =1 -> desligado
    //   fxaa=1        -> suavizacao de serrilhado FXAA
    //   edge=1,hpo=5  -> "Correcao de linhas" ligada: bordas de sprite sem
    //                    vazamento de textura e meio-pixel nativo (sem as linhas
    //                    que o upscale criava no HUD e nos efeitos)
    // Exemplo com tudo padrao: "deinterlace=1,fxaa=0,edge=1,hpo=5".
    // Controles -> variaveis lidas por ps2_pad.cpp (patch 0054). Ex.:
    //   OROCHI_PAD_PLAYER1=0:Xbox Controller   OROCHI_PAD_PLAYER2=auto
    //   OROCHI_KEYBOARD_PLAYER=1
    //   OROCHI_PAD_BUTTONS_PLAYER1=1,3,4,2,7,6,8,5,9,11,10,12,15,13,16,17
    setDefault("OROCHI_PAD_PLAYER1", settings.padPlayer[0]);
    setDefault("OROCHI_PAD_PLAYER2", settings.padPlayer[1]);
    setDefault("OROCHI_KEYBOARD_PLAYER", std::to_string(settings.keyboardPlayer));
    setDefault("OROCHI_PAD_BUTTONS_PLAYER1", orochiPadButtonMapToText(settings.padButtons[0]));
    setDefault("OROCHI_PAD_BUTTONS_PLAYER2", orochiPadButtonMapToText(settings.padButtons[1]));
    // Teclado e mouse (patch 0055). Ex.: OROCHI_KEYBOARD_MAP=265;264;263;262;88,32,75;...
    // OROCHI_MOUSE_BUTTONS=0 desliga os botoes do mouse no jogo.
    setDefault("OROCHI_KEYBOARD_MAP", orochiKeyboardMapToText(settings.keyboardMap));
    setDefault("OROCHI_MOUSE_BUTTONS", settings.mouseButtons ? "1" : "0");
    setDefault("OROCHI_PCSX2_FIXES", std::string("deinterlace=") + (settings.deinterlace ? "0" : "1") +
                                         ",fxaa=" + (settings.fxaa ? "1" : "0") +
                                         (settings.seamFix ? ",edge=1,hpo=5" : ",edge=0"));
    if (!std::getenv("OROCHI_WIDESCREEN"))
        OrochiGamePatches::widescreen = settings.wide();
    // The whole window has to fit, title bar and taskbar included: Windows
    // would cut a taller one short and it would stop being 16:9.
    // Tamanho da janela: o tamanho escolhido no Setup e reduzido (mantendo a
    // proporcao) se nao couber no monitor junto com a barra de titulo (32 px)
    // e a barra de tarefas (48 px). Depois a janela e centralizada.
    // Exemplo: monitor 1920x1080 e janela 1920x1080 -> cabe no maximo
    // 1000 px de altura -> fit = 1000/1080 -> janela 1777x1000.
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
