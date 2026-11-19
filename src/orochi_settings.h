#pragma once
// The settings shared by the two programs: WarriorsOrochi-Setup.exe writes
// them and WarriorsOrochi.exe starts the game with them. They live in
// orochi-settings.ini, next to both executables.
#include <filesystem>
#include <string>
#include <vector>
// Para os mapas de botoes dos controles (16 botoes do PS2 por jogador).
#include <array>

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

// ---------------------------------------------------------------------------
// CONTROLES: botoes do PS2 na ordem usada pelo ini, pelo Setup e pelo runtime
// (vendor/PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp, patch 0054). NAO mude a
// ordem: ela e o formato salvo no orochi-settings.ini.
//   0 cima, 1 baixo, 2 esquerda, 3 direita, 4 X, 5 circulo, 6 quadrado,
//   7 triangulo, 8 L1, 9 R1, 10 L2, 11 R2, 12 Start, 13 Select, 14 L3, 15 R3
// Cada posicao guarda o botao FISICO do controle, no numero do enum
// GamepadButton do raylib: 1 direcional cima, 2 direita, 3 baixo, 4 esquerda,
// 5 botao de cima (Y/triangulo), 6 da direita (B/circulo), 7 de baixo
// (A/X), 8 da esquerda (X/quadrado), 9 LB/L1, 10 LT/L2, 11 RB/R1, 12 RT/R2,
// 13 Back/Select, 14 Guide/PS, 15 Start, 16 analogico esquerdo apertado,
// 17 analogico direito apertado; 0 = nenhum.
// ---------------------------------------------------------------------------
inline constexpr int kOrochiPs2ButtonCount = 16;
using OrochiPadButtonMap = std::array<int, kOrochiPs2ButtonCount>;
// Padrao: o mesmo layout de sempre (A do Xbox = X do PS2, B = circulo...).
inline constexpr OrochiPadButtonMap kOrochiDefaultPadButtonMap = {1, 3, 4, 2, 7, 6, 8, 5, 9, 11, 10, 12, 15, 13, 16, 17};

// Texto <-> mapa, no formato do ini e da variavel OROCHI_PAD_BUTTONS_PLAYERn.
// Ex.: kOrochiDefaultPadButtonMap <-> "1,3,4,2,7,6,8,5,9,11,10,12,15,13,16,17".
// Texto invalido (faltando numero, fora de 0..17) devolve o padrao inteiro.
std::string orochiPadButtonMapToText(const OrochiPadButtonMap &map);
OrochiPadButtonMap orochiPadButtonMapFromText(const std::string &text);

// ---------------------------------------------------------------------------
// TECLADO E MOUSE: mapa de teclas (patch 0055, ps2_pad.cpp). 20 acoes:
//   0..15 = botoes do PS2 na mesma ordem do mapa do controle (acima)
//   16 analogico cima, 17 baixo, 18 esquerda, 19 direita
// Cada acao tem ate 4 codigos (0 = vazio):
//   1..399     tecla (enum KeyboardKey do raylib: 88 = X, 32 = Espaco,
//              265 = seta para cima, 340 = Shift esquerdo...)
//   1000..1006 botao do mouse (1000 + MOUSE_BUTTON_*: 1000 esquerdo,
//              1001 direito, 1002 meio, 1003 lateral, 1004 extra,
//              1005 avancar, 1006 voltar)
//   1010 / 1011 roda do mouse para cima / para baixo
// Formato de texto (ini e OROCHI_KEYBOARD_MAP): acoes separadas por ";",
// codigos por ",". Ex. do comeco do padrao: "265;264;263;262;88,32,75;...".
// O padrao e IGUAL ao kOrochiDefaultKeyboardAndMouseMap do ps2_pad.cpp; se
// mudar um, mude o outro.
// ---------------------------------------------------------------------------
inline constexpr int kOrochiKeyboardActionCount = 20;
inline constexpr int kOrochiCodesPerKeyboardAction = 4;
inline constexpr int kOrochiMouseButtonCodeBase = 1000;
inline constexpr int kOrochiMouseWheelUpCode = 1010;
inline constexpr int kOrochiMouseWheelDownCode = 1011;
using OrochiKeyboardAction = std::array<int, kOrochiCodesPerKeyboardAction>;
using OrochiKeyboardMap = std::array<OrochiKeyboardAction, kOrochiKeyboardActionCount>;
// Teclas de sempre + mouse (esquerdo = quadrado, direito = triangulo,
// meio = circulo). Numeros = KeyboardKey do raylib (comentados ao lado).
inline constexpr OrochiKeyboardMap kOrochiDefaultKeyboardMap = {{
    {265},                 // cima: seta para cima
    {264},                 // baixo: seta para baixo
    {263},                 // esquerda: seta para a esquerda
    {262},                 // direita: seta para a direita
    {88, 32, 75},          // X: X, Espaco, K
    {67, 256, 76, 1002},   // circulo: C, Esc, L, botao do meio do mouse
    {90, 320, 74, 1000},   // quadrado: Z, 0 do teclado numerico, J, clique esquerdo
    {86, 321, 73, 1001},   // triangulo: V, 1 do teclado numerico, I, clique direito
    {81},                  // L1: Q
    {69},                  // R1: E
    {340},                 // L2: Shift esquerdo
    {344, 85},             // R2: Shift direito, U
    {257},                 // Start: Enter
    {258, 259},            // Select: Tab, Backspace
    {},                    // L3: nenhuma
    {},                    // R3: nenhuma
    {87},                  // analogico cima: W
    {83},                  // analogico baixo: S
    {65},                  // analogico esquerda: A
    {68},                  // analogico direita: D
}};
// Texto <-> mapa (texto invalido devolve o padrao inteiro).
std::string orochiKeyboardMapToText(const OrochiKeyboardMap &map);
OrochiKeyboardMap orochiKeyboardMapFromText(const std::string &text);

struct OrochiSettings
{
    // Campos de imagem/resolucao:
    //   pcsx2      : usa o renderizador do PCSX2 (necessario para 16:9 e upscale)
    //   scale      : multiplicador da resolucao interna (1, 2, 3, 4, 5, 6 ou 8)
    //                ex.: 2 -> 1280x896 internos (2 x 640x448 do PS2)
    //   widescreen : 16:9 (true) ou 4:3 (false)
    //   fullscreen : abre em tela cheia sem borda (F10 alterna no jogo)
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
    // Controles (aba Controles do Setup; lidos no jogo por ps2_pad.cpp):
    //   padPlayer[0] / padPlayer[1]: controle do jogador 1 / 2:
    //     "auto" (automatico), "none" (nenhum) ou "<indice>:<nome>",
    //     ex.: "0:Xbox Controller".
    //   keyboardPlayer: 1 ou 2 = jogador que o teclado controla, 0 = desligado.
    //   padButtons[0] / padButtons[1]: mapa de botoes de cada jogador
    //     (formato explicado em kOrochiPs2ButtonCount, acima).
    std::array<std::string, 2> padPlayer{"auto", "auto"};
    int keyboardPlayer = 1;
    std::array<OrochiPadButtonMap, 2> padButtons{kOrochiDefaultPadButtonMap, kOrochiDefaultPadButtonMap};
    // Teclado e mouse (patch 0055): o mapa de teclas e se os botoes do
    // mouse valem no jogo (desligado = mouse ignorado, mapa preservado).
    OrochiKeyboardMap keyboardMap = kOrochiDefaultKeyboardMap;
    bool mouseButtons = true;
    std::string disc; // full path, in UTF-8; empty until one is chosen

    // wide(): 16:9 de fato ativo (so funciona com o renderizador PCSX2).
    // sizes(): lista de tamanhos de janela do formato ativo (16:9 ou 4:3).
    bool wide() const { return pcsx2 && widescreen; }
    const std::vector<OrochiWindowSize> &sizes() const;
};

// Lista de multiplicadores de resolucao oferecidos no Setup.
// Ler / gravar orochi-settings.ini (ao lado dos executaveis).
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
