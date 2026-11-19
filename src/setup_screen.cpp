// WarriorsOrochi-Setup.exe: chooses the disc and the options the game starts
// with, saved to orochi-settings.ini (src/orochi_settings.h). Drawn with raylib.
// The options are grouped in tabs (game, graphics, audio, controls, cheats).
// Compiled with /utf-8: the Portuguese strings below are UTF-8, as raylib's
// text functions expect.
#include "disc_image.h"
#include "orochi_settings.h"
#include "raylib.h"

#include <algorithm>
// std::abs(float), usado na lista de controles conectados.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using Settings = OrochiSettings;
    using Size = OrochiWindowSize;
    constexpr bool kHasPcsx2 = kOrochiHasPcsx2;
    const std::vector<int> &kScales = orochiScales();

    // ---- The screen ----

    enum class Tab
    {
        Game,
        Graphics,
        Audio,
        Controls,
        Cheats,
        Count,
    };

    const char *tabName(Tab tab)
    {
        switch (tab)
        {
        case Tab::Game: return "Jogo";
        case Tab::Graphics: return "Gráficos";
        case Tab::Audio: return "Áudio";
        case Tab::Controls: return "Controles";
        case Tab::Cheats: return "Trapaças";
        default: return "";
        }
    }

    enum class Row
    {
        Disc,
        Browse,
        AutoSave,
        Renderer,
        Scale,
        Aspect,
        Display,
        WindowSize,
        Deinterlace,
        Fxaa,
        SeamFix,
        Crowd,
        Audio,
        Volume,
        CheatInvincible,
        CheatLife,
        CheatMusou,
        CheatPoints,
        // ---- Aba Controles (novas linhas) ----
        // PadPlayer1/2   : qual controle e de cada jogador (Automatico,
        //                  Nenhum ou um controle conectado especifico).
        // KeyboardPlayer : qual jogador o teclado controla (1, 2 ou ninguem).
        // ButtonsPlayer1/2 : abre a tela para trocar os botoes do jogador.
        // KeyboardKeys   : abre a lista de teclas do teclado (so leitura).
        PadPlayer1,
        PadPlayer2,
        KeyboardPlayer,
        ButtonsPlayer1,
        ButtonsPlayer2,
        KeyboardKeys,
        Play,
        Save,
        Quit,
        Count,
    };

    // The tab a row is on; the three buttons are on every tab.
    Tab tabOf(Row row)
    {
        switch (row)
        {
        case Row::Disc:
        case Row::Browse:
        case Row::AutoSave:
            return Tab::Game;
        case Row::Renderer:
        case Row::Scale:
        case Row::Aspect:
        case Row::Display:
        case Row::WindowSize:
        case Row::Deinterlace:
        case Row::Fxaa:
        case Row::SeamFix:
        case Row::Crowd:
            return Tab::Graphics;
        case Row::Audio:
        case Row::Volume:
            return Tab::Audio;
        case Row::CheatInvincible:
        case Row::CheatLife:
        case Row::CheatMusou:
        case Row::CheatPoints:
            return Tab::Cheats;
        // Linhas da aba Controles.
        case Row::PadPlayer1:
        case Row::PadPlayer2:
        case Row::KeyboardPlayer:
        case Row::ButtonsPlayer1:
        case Row::ButtonsPlayer2:
        case Row::KeyboardKeys:
            return Tab::Controls;
        default:
            return Tab::Count;
        }
    }

    // The keyboard (src: ps2_pad.cpp) and the port's own keys, for the Controls tab.
    struct KeyLine
    {
        const char *action, *keys;
    };
    const KeyLine kKeys[] = {
        {"Mover (analógico)", "W A S D"},
        {"Direcional", "Setas"},
        {"X (pular, confirmar)", "X, Espaço ou K"},
        {"Quadrado (ataque normal)", "Z, J ou 0 do teclado numérico"},
        {"Triângulo (ataque carregado)", "V, I ou 1 do teclado numérico"},
        {"Círculo (musou, voltar)", "C, L ou Esc"},
        {"L1 / R1", "Q / E"},
        {"L2 / R2", "Shift esquerdo / Shift direito ou U"},
        {"Start / Select", "Enter / Tab ou Backspace"},
        {"Tela cheia", "F10"},
        {"Screenshot (pasta data)", "F12"},
        {"Fechar o jogo", "Alt+F4"},
    };

    // ---------------------------------------------------------------------
    // CONTROLES: nomes mostrados na tela de botoes
    // ---------------------------------------------------------------------
    // Nome de cada botao do PS2, na ordem do mapa (orochi_settings.h,
    // kOrochiPs2ButtonCount): 0 cima ... 15 R3.
    const char *const kPs2ButtonNamesInMapOrder[kOrochiPs2ButtonCount] = {
        "Direcional ↑", "Direcional ↓", "Direcional ←", "Direcional →",
        "X (pular, confirmar)", "Círculo (musou, voltar)", "Quadrado (ataque normal)",
        "Triângulo (ataque carregado)", "L1", "R1", "L2", "R2", "Start", "Select", "L3", "R3"};

    // Nome do botao FISICO do controle (numero do enum GamepadButton do
    // raylib). Mostra o nome no Xbox e no PlayStation, porque o raylib usa a
    // posicao do botao, nao a letra. Ex.: 7 -> "A / Cruz" (botao de baixo).
    const char *physicalGamepadButtonName(int physicalButton)
    {
        switch (physicalButton)
        {
        case GAMEPAD_BUTTON_LEFT_FACE_UP: return "Direcional ↑";
        case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return "Direcional →";
        case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return "Direcional ↓";
        case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return "Direcional ←";
        case GAMEPAD_BUTTON_RIGHT_FACE_UP: return "Y / Triângulo";
        case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return "B / Círculo";
        case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return "A / Cruz";
        case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return "X / Quadrado";
        case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return "LB / L1";
        case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return "LT / L2";
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return "RB / R1";
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return "RT / R2";
        case GAMEPAD_BUTTON_MIDDLE_LEFT: return "Back / Select";
        case GAMEPAD_BUTTON_MIDDLE: return "Guide / PS";
        case GAMEPAD_BUTTON_MIDDLE_RIGHT: return "Start";
        case GAMEPAD_BUTTON_LEFT_THUMB: return "Analógico esq. (L3)";
        case GAMEPAD_BUTTON_RIGHT_THUMB: return "Analógico dir. (R3)";
        default: return "Nenhum";
        }
    }

    // O mesmo arquivo opcional de mapeamentos que o jogo carrega
    // (ps2_pad.cpp, loadGamepadMappingsOnce): sem ele, o Setup poderia
    // enxergar controles diferentes dos que o jogo enxerga.
    void loadGamepadMappingsLikeTheGame()
    {
        const std::string path = std::string(GetApplicationDirectory()) + "gamecontrollerdb.txt";
        if (!FileExists(path.c_str()))
            return;
        if (char *text = LoadFileText(path.c_str()))
        {
            SetGamepadMappings(text);
            UnloadFileText(text);
        }
    }

    // ---------------------------------------------------------------------
    // TECLADO E MOUSE: nomes mostrados na tela "Teclado e mouse"
    // ---------------------------------------------------------------------
    // Nome de cada acao do mapa de teclado (orochi_settings.h,
    // kOrochiKeyboardActionCount): 0..15 botoes do PS2, 16..19 analogico.
    const char *keyboardActionName(int action)
    {
        static const char *const kStickNames[4] = {"Mover ↑ (analógico)", "Mover ↓ (analógico)",
                                                   "Mover ← (analógico)", "Mover → (analógico)"};
        return action < kOrochiPs2ButtonCount ? kPs2ButtonNamesInMapOrder[action] : kStickNames[action - kOrochiPs2ButtonCount];
    }

    // Nome de um codigo do mapa (tecla do raylib ou botao/roda do mouse).
    // Ex.: 88 -> "X", 32 -> "Espaço", 320 -> "Num 0", 1000 -> "Clique esquerdo",
    // 1010 -> "Roda ↑". Codigo sem nome conhecido -> "Tecla 123".
    std::string keyOrMouseCodeName(int code)
    {
        switch (code)
        {
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_LEFT: return "Clique esquerdo";
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_RIGHT: return "Clique direito";
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_MIDDLE: return "Botão do meio";
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_SIDE: return "Mouse lateral";
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_EXTRA: return "Mouse extra";
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_FORWARD: return "Mouse avançar";
        case kOrochiMouseButtonCodeBase + MOUSE_BUTTON_BACK: return "Mouse voltar";
        case kOrochiMouseWheelUpCode: return "Roda ↑";
        case kOrochiMouseWheelDownCode: return "Roda ↓";
        case KEY_SPACE: return "Espaço";
        case KEY_ESCAPE: return "Esc";
        case KEY_ENTER: return "Enter";
        case KEY_TAB: return "Tab";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_INSERT: return "Insert";
        case KEY_DELETE: return "Delete";
        case KEY_UP: return "Seta ↑";
        case KEY_DOWN: return "Seta ↓";
        case KEY_LEFT: return "Seta ←";
        case KEY_RIGHT: return "Seta →";
        case KEY_PAGE_UP: return "Page Up";
        case KEY_PAGE_DOWN: return "Page Down";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_CAPS_LOCK: return "Caps Lock";
        case KEY_LEFT_SHIFT: return "Shift esq.";
        case KEY_RIGHT_SHIFT: return "Shift dir.";
        case KEY_LEFT_CONTROL: return "Ctrl esq.";
        case KEY_RIGHT_CONTROL: return "Ctrl dir.";
        case KEY_LEFT_ALT: return "Alt esq.";
        case KEY_RIGHT_ALT: return "Alt dir.";
        case KEY_KP_ENTER: return "Enter (num.)";
        case KEY_KP_DECIMAL: return "Num .";
        case KEY_KP_ADD: return "Num +";
        case KEY_KP_SUBTRACT: return "Num -";
        case KEY_KP_MULTIPLY: return "Num *";
        case KEY_KP_DIVIDE: return "Num /";
        case KEY_APOSTROPHE: return "'";
        case KEY_COMMA: return ",";
        case KEY_MINUS: return "-";
        case KEY_PERIOD: return ".";
        case KEY_SLASH: return "/";
        case KEY_SEMICOLON: return ";";
        case KEY_EQUAL: return "=";
        case KEY_LEFT_BRACKET: return "[";
        case KEY_BACKSLASH: return "\\";
        case KEY_RIGHT_BRACKET: return "]";
        case KEY_GRAVE: return "`";
        default: break;
        }
        if ((code >= KEY_A && code <= KEY_Z) || (code >= KEY_ZERO && code <= KEY_NINE))
            return std::string(1, static_cast<char>(code));
        if (code >= KEY_F1 && code <= KEY_F12)
            return "F" + std::to_string(code - KEY_F1 + 1);
        if (code >= KEY_KP_0 && code <= KEY_KP_9)
            return "Num " + std::to_string(code - KEY_KP_0);
        return "Tecla " + std::to_string(code);
    }

    // Todos os codigos de uma acao, separados por virgula. Ex.: acao X do
    // padrao -> "X, Espaço, K"; acao vazia -> "—".
    std::string keyboardActionCodesText(const OrochiKeyboardAction &codes)
    {
        std::string text;
        for (const int code : codes)
            if (code > 0)
                text += (text.empty() ? "" : ", ") + keyOrMouseCodeName(code);
        return text.empty() ? "—" : text;
    }

    // Quantos controles o raylib acompanha (indices 0..3), igual ao jogo.
    constexpr int kSetupMaxGamepads = 4;

    // Texto salvo para "este controle especifico": "<indice>:<nome>".
    // Ex.: controle 0 chamado "Xbox Controller" -> "0:Xbox Controller".
    std::string specificControllerChoiceText(int gamepadIndex)
    {
        return std::to_string(gamepadIndex) + ":" + GetGamepadName(gamepadIndex);
    }

    // Separa "<indice>:<nome>" (valor de padPlayer). Retorna false para
    // "auto"/"none". Ex.: "1:DualSense" -> index 1, name "DualSense".
    bool splitSpecificControllerChoice(const std::string &choice, int &index, std::string &name)
    {
        const size_t colon = choice.find(':');
        if (colon == std::string::npos || colon == 0)
            return false;
        index = std::atoi(choice.c_str());
        name = choice.substr(colon + 1);
        return true;
    }

    // Mesmo criterio do jogo (ps2_pad.cpp, findConnectedGamepadMatchingChoice):
    // mesma posicao e nome; senao, o primeiro com o mesmo nome; senao -1.
    int findConnectedGamepadForChoice(const std::string &choice)
    {
        int index = -1;
        std::string name;
        if (!splitSpecificControllerChoice(choice, index, name))
            return -1;
        if (index >= 0 && index < kSetupMaxGamepads && IsGamepadAvailable(index) && name == GetGamepadName(index))
            return index;
        for (int other = 0; other < kSetupMaxGamepads; ++other)
            if (IsGamepadAvailable(other) && name == GetGamepadName(other))
                return other;
        return -1;
    }

    // Qual controle o JOGO vai dar ao jogador (0 = jogador 1, 1 = jogador 2),
    // com as escolhas atuais do Setup. Copia da regra do jogo
    // (ps2_pad.cpp, gamepadIndexForPlayerPort), sem a checagem de foco:
    //   "none" -> -1; especifico conectado -> ele; automatico (ou especifico
    //   desconectado) -> o proximo controle livre, na ordem dos jogadores.
    int gamepadTheGameWillGivePlayer(const OrochiSettings &settings, int player)
    {
        std::array<int, 2> specificIndex{-1, -1};
        std::array<bool, 2> usesAutomatic{false, false};
        for (int other = 0; other < 2; ++other)
        {
            if (settings.padPlayer[other] == "none")
                continue;
            specificIndex[other] = findConnectedGamepadForChoice(settings.padPlayer[other]);
            usesAutomatic[other] = specificIndex[other] < 0;
        }
        if (settings.padPlayer[player] == "none")
            return -1;
        if (specificIndex[player] >= 0)
            return specificIndex[player];
        const int automaticRank = (player == 1 && usesAutomatic[0]) ? 1 : 0;
        int freeSeen = 0;
        for (int index = 0; index < kSetupMaxGamepads; ++index)
        {
            if (!IsGamepadAvailable(index) || index == specificIndex[0] || index == specificIndex[1])
                continue;
            if (freeSeen == automaticRank)
                return index;
            ++freeSeen;
        }
        return -1;
    }

    enum class Outcome
    {
        Quit,
        Save,
        Play,
    };

    constexpr int kWindowHeight = 760;
    constexpr float kRowsTop = 186, kRowPitch = 46, kPanelBottom = 556;

    struct Fonts
    {
        Font title{}, text{}, small{};
        bool loaded = false;

        void load()
        {
            std::vector<int> codepoints;
            for (int c = 32; c < 256; ++c)
                codepoints.push_back(c);
            for (int c : {0x2014, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193})
                codepoints.push_back(c);
            // "●" (U+25CF): marca o controle que esta sendo apertado na lista
            // "Controles conectados" da aba Controles.
            codepoints.push_back(0x25CF);
            auto open = [&](const char *file, int size) {
                const std::string path = std::string("C:/Windows/Fonts/") + file;
                if (!FileExists(path.c_str()))
                    return GetFontDefault();
                Font font = LoadFontEx(path.c_str(), size, codepoints.data(), static_cast<int>(codepoints.size()));
                SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
                return font;
            };
            title = open("segoeuib.ttf", 44);
            text = open("segoeui.ttf", 26);
            small = open("segoeui.ttf", 20);
            loaded = true;
        }

        void unload()
        {
            for (Font *font : {&title, &text, &small})
                if (font->texture.id != GetFontDefault().texture.id)
                    UnloadFont(*font);
        }
    };

    class Screen
    {
    public:
        Screen(Settings &settings, std::vector<std::filesystem::path> &discs, size_t &disc)
            : m_settings(settings), m_discs(discs), m_disc(disc)
        {
            if (m_settings.disc.empty())
                m_focus = Row::Browse;
        }

        Outcome run()
        {
            m_fonts.load();
            Outcome outcome = Outcome::Quit;
            bool done = false;
            while (!done && !WindowShouldClose())
            {
                done = input(outcome);
                draw();
            }
            m_fonts.unload();
            return outcome;
        }

    private:
        Settings &m_settings;
        std::vector<std::filesystem::path> &m_discs;
        size_t &m_disc;
        Fonts m_fonts;
        Tab m_tab = Tab::Game;
        Row m_focus = Row::Play;
        std::string m_problem;

        // ---- Telas extras da aba Controles ----
        // Page::Main = a tela normal com abas. As outras ocupam a janela toda
        // ate o usuario voltar (Esc, Backspace, B do controle ou "Voltar").
        enum class Page
        {
            Main,
            ButtonsPlayer1, // trocar botoes do jogador 1
            ButtonsPlayer2, // trocar botoes do jogador 2
            KeyboardKeys,   // lista de teclas do teclado (so leitura)
        };
        Page m_page = Page::Main;
        // Na tela de botoes: qual botao do PS2 esta selecionado (0..15, ordem
        // de kPs2ButtonNamesInMapOrder) e se estamos esperando o usuario
        // apertar o novo botao no controle.
        int m_buttonFocus = 0;
        bool m_waitingForNewButton = false;
        // Valor anterior dos gatilhos de cada controle, para perceber o
        // momento em que um gatilho e apertado (eixo passa de 0.5).
        mutable std::array<std::array<float, 2>, kSetupMaxGamepads> m_previousTriggerAxes{};
        // Ultima vez (GetTime, segundos) em que cada controle teve um botao
        // apertado: a lista "Controles conectados" acende o controle por 0,6 s.
        mutable std::array<double, kSetupMaxGamepads> m_lastGamepadActivity{-10.0, -10.0, -10.0, -10.0};
        // Tela "Teclado e mouse":
        //   m_keyboardFocus      : acao selecionada (0..19, ver keyboardActionName)
        //   m_waitingForNewKey   : esperando a tecla/botao do mouse novo
        //   m_newKeyIsAddition   : true = adiciona (Insert); false = substitui (Enter)
        //   m_keyboardNotice     : aviso curto, ex.: "Espaço saiu de X (pular)"
        int m_keyboardFocus = 0;
        bool m_waitingForNewKey = false;
        bool m_newKeyIsAddition = false;
        std::string m_keyboardNotice;

        // Retangulo da acao "action" na tela de teclado: duas colunas de 10.
        Rectangle keyboardActionRect(int action) const
        {
            const float k = scale();
            const int column = action / 10, line = action % 10;
            return {k * (60 + column * 425), k * (kRowsTop + line * 36), k * 415, k * 32};
        }

        // Grava "code" na acao selecionada.
        //   - O codigo sai de qualquer outra acao que o usasse (uma tecla faz
        //     uma coisa so). Se isso acontecer, m_keyboardNotice avisa.
        //   - Substituir: a acao fica so com esse codigo.
        //   - Adicionar: entra na primeira posicao vazia; sem vaga (4 ja
        //     usadas), substitui a ultima.
        // Ex.: X tem "X, Espaço, K"; Enter + tecla N -> X tem so "N".
        //      Insert + clique esquerdo -> "X, Espaço, K, Clique esquerdo" e o
        //      clique sai do quadrado.
        void assignCodeToFocusedKeyboardAction(int code)
        {
            OrochiKeyboardMap &map = m_settings.keyboardMap;
            m_keyboardNotice.clear();
            for (int action = 0; action < kOrochiKeyboardActionCount; ++action)
                for (int &existing : map[action])
                    if (existing == code && action != m_keyboardFocus)
                    {
                        existing = 0;
                        m_keyboardNotice = keyOrMouseCodeName(code) + " saiu de \"" + keyboardActionName(action) + "\".";
                    }
            OrochiKeyboardAction &codes = map[m_keyboardFocus];
            if (!m_newKeyIsAddition)
            {
                codes = {code, 0, 0, 0};
                return;
            }
            if (std::find(codes.begin(), codes.end(), code) != codes.end())
                return; // ja estava nesta acao
            for (int &slot : codes)
                if (slot == 0)
                {
                    slot = code;
                    return;
                }
            codes.back() = code;
        }

        // Enquanto espera: a tecla, botao do mouse ou roda apertado agora
        // (codigo do mapa), ou 0. F9, F10 e F12 sao atalhos do port e nao
        // podem virar acao do jogo: devolvem -1 (aviso na tela).
        int keyOrMouseCodePressedNow() const
        {
            for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
            {
                if (key == KEY_F9 || key == KEY_F10 || key == KEY_F12)
                    return -1;
                if (key > 0 && key <= 399 && key != KEY_ESCAPE)
                    return key;
            }
            for (int button = MOUSE_BUTTON_LEFT; button <= MOUSE_BUTTON_BACK; ++button)
                if (IsMouseButtonPressed(button))
                    return kOrochiMouseButtonCodeBase + button;
            const float wheel = GetMouseWheelMove();
            if (wheel > 0.0f)
                return kOrochiMouseWheelUpCode;
            if (wheel < 0.0f)
                return kOrochiMouseWheelDownCode;
            return 0;
        }

        // Entrada da tela "Teclado e mouse".
        void inputKeyboardAndMousePage()
        {
            const bool pad = IsGamepadAvailable(0);
            auto pressed = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
            auto padPressed = [pad](int button) { return pad && IsGamepadButtonPressed(0, button); };
            const Vector2 mouse = GetMousePosition();
            const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

            if (m_waitingForNewKey)
            {
                // Esc cancela; qualquer outra tecla, botao do mouse ou roda e gravado.
                if (IsKeyPressed(KEY_ESCAPE))
                {
                    m_waitingForNewKey = false;
                    return;
                }
                const int code = keyOrMouseCodePressedNow();
                if (code == -1)
                    m_keyboardNotice = "F9, F10 e F12 são atalhos do port (captura, tela cheia, screenshot).";
                else if (code > 0)
                {
                    assignCodeToFocusedKeyboardAction(code);
                    m_waitingForNewKey = false;
                }
                return;
            }

            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                (click && CheckCollisionPointRec(mouse, backButtonRect())))
            {
                closeSubPage();
                return;
            }
            // Navegacao: ↑↓ dentro da coluna de 10, ←→ troca de coluna.
            if (pressed(KEY_DOWN) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                m_keyboardFocus = (m_keyboardFocus + 1) % kOrochiKeyboardActionCount;
            if (pressed(KEY_UP) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP))
                m_keyboardFocus = (m_keyboardFocus + kOrochiKeyboardActionCount - 1) % kOrochiKeyboardActionCount;
            if (pressed(KEY_RIGHT) || pressed(KEY_LEFT) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ||
                padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                m_keyboardFocus = (m_keyboardFocus + 10) % kOrochiKeyboardActionCount;
            // Delete: esta acao volta ao padrao. R: todas. M: liga/desliga o mouse.
            if (IsKeyPressed(KEY_DELETE))
            {
                m_settings.keyboardMap[m_keyboardFocus] = kOrochiDefaultKeyboardMap[m_keyboardFocus];
                m_keyboardNotice.clear();
            }
            if (IsKeyPressed(KEY_R))
            {
                m_settings.keyboardMap = kOrochiDefaultKeyboardMap;
                m_keyboardNotice.clear();
            }
            if (IsKeyPressed(KEY_M))
                m_settings.mouseButtons = !m_settings.mouseButtons;
            // Enter/Espaco/A/clique: substituir. Insert: adicionar. A tecla ou
            // clique que abriu a espera nao conta (a fila de teclas e
            // esvaziada aqui e o clique so vale neste quadro).
            const bool replace = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE) ||
                                 padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
            const bool add = IsKeyPressed(KEY_INSERT);
            for (int action = 0; action < kOrochiKeyboardActionCount; ++action)
                if (CheckCollisionPointRec(mouse, keyboardActionRect(action)))
                {
                    if (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f)
                        m_keyboardFocus = action;
                    if (click)
                    {
                        m_keyboardFocus = action;
                        m_newKeyIsAddition = false;
                        m_waitingForNewKey = true;
                    }
                }
            if (replace || add)
            {
                m_newKeyIsAddition = add;
                m_waitingForNewKey = true;
            }
            if (m_waitingForNewKey)
                while (GetKeyPressed() != 0)
                {
                }
        }

        // Desenho do conteudo da tela "Teclado e mouse" (dentro do painel) e
        // o texto de ajuda; o resto (titulo, Voltar) e de drawSubPage.
        std::string drawKeyboardAndMousePage(float k) const
        {
            const Color gold{226, 186, 92, 255}, ink{232, 234, 240, 255}, focus{60, 82, 130, 255};
            for (int action = 0; action < kOrochiKeyboardActionCount; ++action)
            {
                const Rectangle r = keyboardActionRect(action);
                const bool focused = action == m_keyboardFocus;
                if (focused)
                    DrawRectangleRounded(r, 0.2f, 6, m_waitingForNewKey ? Color{130, 90, 40, 255} : focus);
                text(m_fonts.small, keyboardActionName(action), r.x + k * 12, r.y + k * 6, k * 19, ink);
                const bool changed = m_settings.keyboardMap[action] != kOrochiDefaultKeyboardMap[action];
                const std::string shown = focused && m_waitingForNewKey ? (m_newKeyIsAddition ? "adicionar: aperte…" : "aperte…")
                                                                        : keyboardActionCodesText(m_settings.keyboardMap[action]);
                // Texto longo encolhe para caber na metade direita da linha.
                const float room = r.width * 0.55f, wide = MeasureTextEx(m_fonts.small, shown.c_str(), k * 19, 0.0f).x;
                const float size = k * 19 * std::min(1.0f, room / std::max(wide, 1.0f));
                const float width = MeasureTextEx(m_fonts.small, shown.c_str(), size, 0.0f).x;
                text(m_fonts.small, shown, r.x + r.width - width - k * 12, r.y + k * 6, size, changed ? gold : ink);
            }
            // Estado do mouse, acima do painel, a direita do titulo.
            const std::string mouseState = std::string("Mouse no jogo: ") + (m_settings.mouseButtons ? "ligado" : "desligado") + " (M)";
            const float mouseWidth = MeasureTextEx(m_fonts.small, mouseState.c_str(), k * 20, 0.0f).x;
            text(m_fonts.small, mouseState, k * 900 - mouseWidth, k * 128, k * 20, m_settings.mouseButtons ? gold : ink);
            if (m_waitingForNewKey)
                return std::string("Aperte a tecla, o botão do mouse ou gire a roda para \"") + keyboardActionName(m_keyboardFocus) +
                       "\". Esc cancela." + (m_keyboardNotice.empty() ? "" : " " + m_keyboardNotice);
            std::string about = "Enter: trocar (só a nova tecla). Insert: adicionar mais uma (até 4). Delete: padrão desta ação. "
                                "R: restaura todas. M: liga/desliga o mouse. Atalhos fixos: F10 tela cheia, F12 screenshot, Alt+F4 fechar.";
            if (!m_keyboardNotice.empty())
                about = m_keyboardNotice + " " + about;
            return about;
        }

        // Abre uma tela extra. Esc deixa de fechar o Setup (raylib fecha a
        // janela com Esc por padrao) e passa a significar "voltar".
        void openSubPage(Page page)
        {
            m_page = page;
            m_buttonFocus = 0;
            m_waitingForNewButton = false;
            SetExitKey(KEY_NULL);
        }

        // Volta para a tela de abas, com o foco na linha que abriu a tela extra.
        void closeSubPage()
        {
            m_focus = m_page == Page::ButtonsPlayer1 ? Row::ButtonsPlayer1
                    : m_page == Page::ButtonsPlayer2 ? Row::ButtonsPlayer2
                                                     : Row::KeyboardKeys;
            m_page = Page::Main;
            m_waitingForNewButton = false;
            SetExitKey(KEY_ESCAPE);
        }

        // Jogador (0 ou 1) da tela de botoes aberta.
        int playerOfButtonsPage() const { return m_page == Page::ButtonsPlayer2 ? 1 : 0; }

        // Grava "physicalButton" para o botao do PS2 selecionado. Se outro
        // botao do PS2 ja usava esse botao fisico, os dois TROCAM (assim
        // nunca ha dois botoes do PS2 no mesmo botao do controle).
        // Ex.: padrao (X=A, quadrado=X do Xbox); escolher "X do Xbox" para o
        // X do PS2 -> X=X do Xbox e quadrado=A.
        void assignPhysicalButtonToFocusedPs2Button(int physicalButton)
        {
            OrochiPadButtonMap &map = m_settings.padButtons[playerOfButtonsPage()];
            const int previous = map[m_buttonFocus];
            for (int index = 0; index < kOrochiPs2ButtonCount; ++index)
                if (index != m_buttonFocus && map[index] == physicalButton)
                    map[index] = previous;
            map[m_buttonFocus] = physicalButton;
        }

        // Enquanto espera o novo botao: devolve o botao fisico apertado agora
        // em QUALQUER controle conectado, ou 0 se nada foi apertado neste
        // quadro. Gatilhos contam pelo eixo (controles XInput).
        int physicalButtonPressedOnAnyGamepad() const
        {
            int pressed = GAMEPAD_BUTTON_UNKNOWN;
            for (int pad = 0; pad < kSetupMaxGamepads; ++pad)
            {
                if (!IsGamepadAvailable(pad))
                    continue;
                for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB && !pressed; ++button)
                    if (IsGamepadButtonPressed(pad, button))
                        pressed = button;
                const float left = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_TRIGGER);
                const float right = GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_TRIGGER);
                if (!pressed && left > 0.5f && m_previousTriggerAxes[pad][0] <= 0.5f)
                    pressed = GAMEPAD_BUTTON_LEFT_TRIGGER_2;
                if (!pressed && right > 0.5f && m_previousTriggerAxes[pad][1] <= 0.5f)
                    pressed = GAMEPAD_BUTTON_RIGHT_TRIGGER_2;
                m_previousTriggerAxes[pad] = {left, right};
            }
            return pressed;
        }

        // Retangulo do botao do PS2 "index" na tela de botoes: duas colunas
        // de 8 (0..7 a esquerda, 8..15 a direita).
        Rectangle ps2ButtonRect(int index) const
        {
            const float k = scale();
            const int column = index / 8, line = index % 8;
            return {k * (60 + column * 425), k * (kRowsTop + line * 44), k * 415, k * 40};
        }
        // Botao "Voltar" das telas extras (mesmo lugar do botao "Salvar").
        Rectangle backButtonRect() const
        {
            const float k = scale();
            return {k * 395, k * 664, k * 170, k * 52};
        }

        // Teclado/controle/mouse das telas extras.
        void inputSubPage()
        {
            // A tela "Teclado e mouse" tem entrada propria (captura de teclas).
            if (m_page == Page::KeyboardKeys)
            {
                inputKeyboardAndMousePage();
                return;
            }
            const bool pad = IsGamepadAvailable(0);
            auto pressed = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
            auto padPressed = [pad](int button) { return pad && IsGamepadButtonPressed(0, button); };
            const Vector2 mouse = GetMousePosition();
            const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

            if (m_waitingForNewButton)
            {
                // Esperando o novo botao: Esc cancela; qualquer botao do
                // controle e gravado (inclusive B, Start etc.).
                if (IsKeyPressed(KEY_ESCAPE) || click)
                {
                    m_waitingForNewButton = false;
                    return;
                }
                if (const int physical = physicalButtonPressedOnAnyGamepad())
                {
                    assignPhysicalButtonToFocusedPs2Button(physical);
                    m_waitingForNewButton = false;
                }
                return;
            }
            // Mantem os gatilhos atualizados mesmo fora da espera, para que um
            // gatilho ja apertado nao conte como "apertado agora" depois.
            physicalButtonPressedOnAnyGamepad();

            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                (click && CheckCollisionPointRec(mouse, backButtonRect())))
            {
                closeSubPage();
                return;
            }
            if (m_page == Page::KeyboardKeys)
                return;

            // Navegacao: ↑↓ dentro da coluna, ←→ troca de coluna.
            if (pressed(KEY_DOWN) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                m_buttonFocus = (m_buttonFocus + 1) % kOrochiPs2ButtonCount;
            if (pressed(KEY_UP) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP))
                m_buttonFocus = (m_buttonFocus + kOrochiPs2ButtonCount - 1) % kOrochiPs2ButtonCount;
            if (pressed(KEY_RIGHT) || pressed(KEY_LEFT) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT) ||
                padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                m_buttonFocus = (m_buttonFocus + 8) % kOrochiPs2ButtonCount;
            OrochiPadButtonMap &map = m_settings.padButtons[playerOfButtonsPage()];
            // Delete: so este botao volta ao padrao. R: todos voltam ao padrao.
            if (IsKeyPressed(KEY_DELETE))
            {
                const int wanted = kOrochiDefaultPadButtonMap[m_buttonFocus];
                assignPhysicalButtonToFocusedPs2Button(wanted);
            }
            if (IsKeyPressed(KEY_R))
                map = kOrochiDefaultPadButtonMap;
            // Enter/Espaco/A do controle 1 ou clique: comeca a esperar o novo
            // botao. O aperto que abriu a espera nao conta: a leitura so olha
            // botoes apertados nos quadros seguintes.
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE) ||
                padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
                m_waitingForNewButton = true;
            for (int index = 0; index < kOrochiPs2ButtonCount; ++index)
                if (CheckCollisionPointRec(mouse, ps2ButtonRect(index)))
                {
                    if (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f)
                        m_buttonFocus = index;
                    if (click)
                    {
                        m_buttonFocus = index;
                        m_waitingForNewButton = true;
                    }
                }
        }

        // Desenha as telas extras (cabecalho, conteudo, ajuda e "Voltar").
        void drawSubPage() const
        {
            const float k = scale();
            const Color gold{226, 186, 92, 255}, ink{232, 234, 240, 255}, dim{120, 126, 140, 255},
                focus{60, 82, 130, 255}, panel{20, 26, 40, 230};
            BeginDrawing();
            DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(), Color{14, 18, 30, 255}, Color{40, 22, 30, 255});
            text(m_fonts.title, "WARRIORS OROCHI", k * 60, k * 30, k * 44, gold);
            const char *title = m_page == Page::ButtonsPlayer1   ? "Controles — botões do jogador 1"
                              : m_page == Page::ButtonsPlayer2 ? "Controles — botões do jogador 2"
                                                               // Texto original desativado:
                                                               // : "Controles — teclas do teclado";
                                                               : "Controles — teclado e mouse";
            text(m_fonts.text, title, k * 60, k * 122, k * 26, gold);
            DrawRectangleRec({k * 50, k * 168, k * 860, k * (kPanelBottom - 168)}, panel);

            std::string about;
            if (m_page == Page::KeyboardKeys)
            {
                // Bloco original desativado: a tela so listava as teclas fixas
                // (kKeys). Agora mostra e edita o mapa de teclado e mouse.
                // // A lista que antes ficava direto na aba Controles.
                // float y = kRowsTop - 6;
                // for (const KeyLine &line : kKeys)
                // {
                //     text(m_fonts.small, line.action, k * 76, k * y, k * 20, ink);
                //     text(m_fonts.small, line.keys, k * 460, k * y, k * 20, gold);
                //     y += 29;
                // }
                // about = "O teclado controla o jogador escolhido em \"Teclado controla\". As teclas ainda não podem "
                //         "ser trocadas.";
                about = drawKeyboardAndMousePage(k);
            }
            else
            {
                const OrochiPadButtonMap &map = m_settings.padButtons[playerOfButtonsPage()];
                for (int index = 0; index < kOrochiPs2ButtonCount; ++index)
                {
                    const Rectangle r = ps2ButtonRect(index);
                    const bool focused = index == m_buttonFocus;
                    if (focused)
                        DrawRectangleRounded(r, 0.2f, 6, m_waitingForNewButton ? Color{130, 90, 40, 255} : focus);
                    text(m_fonts.small, kPs2ButtonNamesInMapOrder[index], r.x + k * 12, r.y + k * 9, k * 20, ink);
                    const bool changed = map[index] != kOrochiDefaultPadButtonMap[index];
                    const std::string shown = focused && m_waitingForNewButton ? "aperte…" : physicalGamepadButtonName(map[index]);
                    const float width = MeasureTextEx(m_fonts.small, shown.c_str(), k * 20, 0.0f).x;
                    text(m_fonts.small, shown, r.x + r.width - width - k * 12, r.y + k * 9, k * 20, changed ? gold : ink);
                }
                about = m_waitingForNewButton
                            ? std::string("Aperte no controle o botão que vai fazer \"") +
                                  kPs2ButtonNamesInMapOrder[m_buttonFocus] + "\". Esc cancela."
                            : "Enter (ou A / clique): trocar o botão escolhido. Se o novo botão já fizer outra ação, "
                              "as duas trocam de lugar. Delete: volta este ao padrão. R: restaura todos. Em dourado: "
                              "botões diferentes do padrão.";
            }
            wrapped(m_fonts.small, about, k * 60, k * 572, k * 840, k * 20, ink);
            const Rectangle back = backButtonRect();
            DrawRectangleRounded(back, 0.25f, 6, CheckCollisionPointRec(GetMousePosition(), back) ? focus : Color{44, 50, 66, 255});
            centered(m_fonts.text, "Voltar", back, k * 26, ink);
            // Linhas originais desativadas: a tela de teclado tem atalhos a mais
            // (Insert, M), entao a dica do rodape depende da tela.
            // text(m_fonts.small, "↑↓←→ escolher    Enter trocar    Delete padrão    R restaurar todos    Esc voltar",
            //      k * 60, k * 730, k * 18, dim);
            text(m_fonts.small,
                 m_page == Page::KeyboardKeys
                     ? "↑↓←→ escolher    Enter trocar    Insert adicionar    Delete padrão    R restaurar    M mouse    Esc voltar"
                     : "↑↓←→ escolher    Enter trocar    Delete padrão    R restaurar todos    Esc voltar",
                 k * 60, k * 730, k * 18, dim);
            EndDrawing();
        }

        // Lista "Controles conectados" na aba Controles, abaixo das linhas.
        // Cada controle acende (dourado, com ●) enquanto um botao dele e
        // apertado e por 0,6 s depois: e assim que o usuario descobre qual
        // controle fisico e o "1", o "2"... Ex. de linha:
        //   "● 1 · Xbox Controller — jogador 1"
        void drawConnectedControllersList(float k) const
        {
            const Color gold{226, 186, 92, 255}, ink{232, 234, 240, 255}, dim{120, 126, 140, 255};
            float y = kRowsTop + 6 * kRowPitch + 8; // logo abaixo das 6 linhas da aba
            bool any = false;
            for (int pad = 0; pad < kSetupMaxGamepads; ++pad)
            {
                if (!IsGamepadAvailable(pad))
                    continue;
                any = true;
                bool active = false;
                for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
                    active = active || IsGamepadButtonDown(pad, button);
                for (int axis = GAMEPAD_AXIS_LEFT_X; axis <= GAMEPAD_AXIS_RIGHT_TRIGGER; ++axis)
                {
                    const float movement = GetGamepadAxisMovement(pad, axis);
                    // Gatilhos ficam em -1 soltos; analogicos em 0.
                    active = active || (axis >= GAMEPAD_AXIS_LEFT_TRIGGER ? movement > 0.0f : std::abs(movement) > 0.5f);
                }
                if (active)
                    m_lastGamepadActivity[pad] = GetTime();
                const bool lit = GetTime() - m_lastGamepadActivity[pad] < 0.6;
                const int player = gamepadTheGameWillGivePlayer(m_settings, 0) == pad   ? 1
                                 : gamepadTheGameWillGivePlayer(m_settings, 1) == pad ? 2
                                                                                     : 0;
                const std::string line = std::string(lit ? "● " : "   ") + std::to_string(pad + 1) + " · " +
                                         GetGamepadName(pad) + " — " +
                                         (player ? "jogador " + std::to_string(player) : std::string("sem jogador"));
                text(m_fonts.small, line, k * 76, k * y, k * 20, lit ? gold : ink);
                y += 22;
            }
            if (!any)
                text(m_fonts.small, "Nenhum controle conectado (o teclado continua funcionando).", k * 76, k * y, k * 20, dim);
        }

        static bool isButton(Row row) { return row == Row::Play || row == Row::Save || row == Row::Quit; }

        // Rows on the current tab, in order, then the buttons.
        std::vector<Row> visible() const
        {
            std::vector<Row> rows;
            for (int index = 0; index < static_cast<int>(Row::Count); ++index)
            {
                const Row row = static_cast<Row>(index);
                if (tabOf(row) == m_tab || isButton(row))
                    rows.push_back(row);
            }
            return rows;
        }

        bool enabled(Row row) const
        {
            switch (row)
            {
            case Row::Disc:
                return m_discs.size() > 1;
            case Row::Play:
                return !m_settings.disc.empty();
            case Row::Renderer:
                return kHasPcsx2;
            case Row::Scale:
            case Row::Aspect:
            case Row::Deinterlace:
            case Row::Fxaa:
            case Row::SeamFix:
                return m_settings.pcsx2;
            case Row::WindowSize:
                return !m_settings.fullscreen;
            case Row::Volume:
                return m_settings.audio;
            default:
                return true;
            }
        }

        static const char *label(Row row)
        {
            switch (row)
            {
            case Row::Disc: return "Jogo (ISO)";
            case Row::Browse: return "Selecionar outra ISO";
            case Row::AutoSave: return "Save automático";
            case Row::Renderer: return "Renderizador";
            case Row::Scale: return "Resolução interna";
            case Row::Aspect: return "Proporção da tela";
            case Row::Display: return "Modo de exibição";
            case Row::WindowSize: return "Tamanho da janela";
            case Row::Deinterlace: return "Desentrelaçamento";
            case Row::Fxaa: return "Suavização (FXAA)";
            case Row::SeamFix: return "Correção de linhas";
            case Row::Crowd: return "Soldados desenhados";
            case Row::Audio: return "Som do jogo";
            case Row::Volume: return "Volume";
            case Row::CheatInvincible: return "Invencível";
            case Row::CheatLife: return "Vida infinita";
            case Row::CheatMusou: return "Musou infinito";
            case Row::CheatPoints: return "Pontos infinitos";
            case Row::PadPlayer1: return "Controle do jogador 1";
            case Row::PadPlayer2: return "Controle do jogador 2";
            case Row::KeyboardPlayer: return "Teclado controla";
            case Row::ButtonsPlayer1: return "Botões do jogador 1";
            case Row::ButtonsPlayer2: return "Botões do jogador 2";
            // Linha original desativada: a tela agora tambem edita teclas e mouse.
            // case Row::KeyboardKeys: return "Teclas do teclado";
            case Row::KeyboardKeys: return "Teclado e mouse";
            default: return "";
            }
        }

        // Texto do valor das linhas "Controle do jogador N" (player 0 ou 1).
        // Exemplos:
        //   "auto" com um controle livre -> "Automático: 1 · Xbox Controller"
        //   "none"                        -> "Nenhum"
        //   "0:Xbox Controller" conectado -> "1 · Xbox Controller"
        //   desconectado                  -> "Xbox Controller (desconectado)"
        // O numero mostrado e o indice + 1 (o usuario conta a partir de 1),
        // o mesmo da lista "Controles conectados" da aba.
        std::string controllerChoiceText(int player) const
        {
            const std::string &choice = m_settings.padPlayer[player];
            if (choice == "none")
                return "Nenhum";
            const int given = gamepadTheGameWillGivePlayer(m_settings, player);
            const std::string givenText =
                given >= 0 ? std::to_string(given + 1) + " · " + GetGamepadName(given) : std::string("nenhum conectado");
            if (choice == "auto")
                return "Automático: " + givenText;
            int index = -1;
            std::string name;
            splitSpecificControllerChoice(choice, index, name);
            return findConnectedGamepadForChoice(choice) >= 0 ? givenText : name + " (desconectado)";
        }

        // Opcoes para "Controle do jogador N", na ordem em que ←/→ passam:
        // Automatico, Nenhum, cada controle conectado e, se o escolhido estiver
        // desconectado, ele mesmo (para nao perder a escolha so de passar por ela).
        std::vector<std::string> controllerChoicesFor(int player) const
        {
            std::vector<std::string> choices{"auto", "none"};
            for (int index = 0; index < kSetupMaxGamepads; ++index)
                if (IsGamepadAvailable(index))
                    choices.push_back(specificControllerChoiceText(index));
            const std::string &current = m_settings.padPlayer[player];
            if (std::find(choices.begin(), choices.end(), current) == choices.end())
                choices.push_back(current);
            return choices;
        }

        std::string value(Row row) const
        {
            const Settings &s = m_settings;
            switch (row)
            {
            case Row::Disc: {
                if (s.disc.empty())
                    return "Nenhuma ISO selecionada";
                return orochiUtf8(orochiPathFromUtf8(s.disc).stem());
            }
            case Row::Browse:
                return "Procurar no computador…";
            case Row::AutoSave:
                return s.autoSave ? "Ligado" : "Desligado";
            case Row::Renderer:
                return s.pcsx2 ? "PCSX2 (recomendado)" : "Próprio do port";
            case Row::Scale:
                if (!s.pcsx2)
                    return "640x448 (só no PCSX2)";
                return std::to_string(s.scale) + "x — " + std::to_string(640 * s.scale) + "x" +
                       std::to_string(448 * s.scale) + (s.scale == 1 ? " (nativa)" : "");
            case Row::Aspect:
                if (!s.pcsx2)
                    return "4:3 (só no PCSX2)";
                return s.widescreen ? "16:9 (widescreen)" : "4:3 (original)";
            case Row::Display:
                return s.fullscreen ? "Tela cheia (sem bordas)" : "Janela";
            case Row::WindowSize: {
                const Size size = s.sizes()[s.windowSize];
                return std::to_string(size.width) + " x " + std::to_string(size.height);
            }
            case Row::Deinterlace:
                return s.deinterlace ? "Automático" : "Desligado (recomendado)";
            case Row::Fxaa:
                return s.fxaa ? "Ligada" : "Desligada";
            case Row::SeamFix:
                return s.seamFix ? "Ligada (recomendado)" : "Desligada";
            case Row::Crowd:
                return s.crowd <= 51 ? "Original (até 51)" : s.crowd >= 147 ? "Máximo (até 147)" : "Mais (até 100)";
            case Row::Audio:
                return s.audio ? "Ligado" : "Desligado";
            case Row::Volume:
                return std::to_string(s.volume) + "%";
            case Row::CheatInvincible:
                return s.cheatInvincible ? "Ligada" : "Desligada";
            case Row::CheatLife:
                return s.cheatLife ? "Ligada" : "Desligada";
            case Row::CheatMusou:
                return s.cheatMusou ? "Ligada" : "Desligada";
            case Row::CheatPoints:
                return s.cheatPoints ? "Ligada" : "Desligada";
            case Row::PadPlayer1:
                return controllerChoiceText(0);
            case Row::PadPlayer2:
                return controllerChoiceText(1);
            case Row::KeyboardPlayer:
                return s.keyboardPlayer == 2 ? "Jogador 2" : s.keyboardPlayer == 0 ? "Ninguém (desligado)" : "Jogador 1";
            case Row::ButtonsPlayer1:
            case Row::ButtonsPlayer2: {
                // "Padrão" ou quantos botoes foram trocados. Ex.: "2 trocados".
                const OrochiPadButtonMap &map = s.padButtons[row == Row::ButtonsPlayer1 ? 0 : 1];
                int changed = 0;
                for (int index = 0; index < kOrochiPs2ButtonCount; ++index)
                    changed += map[index] != kOrochiDefaultPadButtonMap[index];
                return (changed == 0 ? std::string("Padrão") : std::to_string(changed) + " trocado" + (changed > 1 ? "s" : "")) +
                       " — Enter para configurar";
            }
            // Linhas originais desativadas: a linha agora resume o mapa de teclas.
            // case Row::KeyboardKeys:
            //     return "Enter para ver";
            case Row::KeyboardKeys: {
                // Ex.: "Padrão · mouse ligado — Enter para configurar" ou
                //      "3 trocadas · mouse desligado — Enter para configurar".
                int changed = 0;
                for (int action = 0; action < kOrochiKeyboardActionCount; ++action)
                    changed += s.keyboardMap[action] != kOrochiDefaultKeyboardMap[action];
                return (changed == 0 ? std::string("Padrão") : std::to_string(changed) + " trocada" + (changed > 1 ? "s" : "")) +
                       (s.mouseButtons ? " · mouse ligado" : " · mouse desligado") + " — Enter para configurar";
            }
            default:
                return "";
            }
        }

        static const char *help(Row row)
        {
            switch (row)
            {
            case Row::Disc:
                return "Qual ISO o jogo abre. Aqui aparecem as ISOs compatíveis da mesma pasta, como a "
                       "tradução em português. Os saves são os mesmos para todas.";
            case Row::Browse:
                return "Escolha a ISO do Warriors Orochi (USA, SLUS-21662) em qualquer pasta ou disco. "
                       "O nome do arquivo não importa: o Setup confere se o jogo é compatível.";
            case Row::AutoSave:
                return "Ao fim de cada batalha o jogo salva sozinho, sem perguntar. O save anterior fica "
                       "guardado na pasta mc0-backup. Durante a batalha, salve pelo menu de pausa.";
            case Row::Renderer:
                return "PCSX2: o GS do PCSX2 em Direct3D 11, com resolução maior e 16:9. "
                       "Próprio: o renderizador original do port, em 640x448.";
            case Row::Scale:
                return "Resolução em que o PCSX2 desenha o jogo. Mais alta deixa a imagem mais "
                       "nítida e exige mais da placa de vídeo.";
            case Row::Aspect:
                return "16:9 usa o patch widescreen do jogo (visão mais larga). "
                       "Menus 2D e vídeos ficam esticados.";
            case Row::Display:
                return "Tela cheia sem bordas cobre o monitor inteiro sem trocar a resolução do Windows. "
                       "Durante o jogo, F10 alterna entre tela cheia e janela.";
            case Row::WindowSize:
                return "Tamanho inicial da janela. Ela pode ser redimensionada ou maximizada durante o jogo.";
            case Row::Deinterlace:
                return "Desligado deixa a imagem e o texto bem mais nítidos. Automático mistura os campos "
                       "como o PCSX2 faz, com leve borrão.";
            case Row::Fxaa:
                return "Suaviza o serrilhado das bordas com um filtro leve de pós-processamento.";
            case Row::SeamFix:
                return "Remove linhas finas que aparecem em resolução acima da nativa: bordas de "
                       "sprites do HUD e emendas nos efeitos de tela cheia. Sem efeito em 1x.";
            case Row::Crowd:
                return "Quantos soldados e oficiais o jogo desenha de corpo inteiro. No original, os mais "
                       "distantes aparecem só com a barra de vida. Mais soldados pesam um pouco no desempenho.";
            case Row::Audio:
                return "Som do jogo pela emulação do SPU2 (sem reverb ainda). Desligado, o jogo fica mudo.";
            case Row::Volume:
                return "Volume geral do jogo e dos vídeos, de 0 a 100%.";
            case Row::CheatInvincible:
                return "Os golpes não tiram vida do seu personagem (jogadores 1 e 2). Ele ainda é "
                       "derrubado e empurrado normalmente.";
            case Row::CheatLife:
                return "A barra de vida volta a ficar cheia a todo momento durante a batalha.";
            case Row::CheatMusou:
                return "A barra de musou fica sempre cheia: o ataque musou dura enquanto o botão "
                       "estiver apertado.";
            case Row::CheatPoints:
                return "Pontos de crescimento sempre em 99999 no acampamento. O save guarda o "
                       "valor que estiver lá ao salvar.";
            case Row::PadPlayer1:
            case Row::PadPlayer2:
                return "Qual controle é deste jogador. Automático: o 1º controle livre vai para o jogador 1 e o "
                       "seguinte para o jogador 2. Aperte um botão no controle para ver na lista abaixo qual é ele.";
            case Row::KeyboardPlayer:
                return "Qual jogador o teclado controla, junto com o controle desse jogador. Útil para jogar a "
                       "dois com um só controle.";
            case Row::ButtonsPlayer1:
            case Row::ButtonsPlayer2:
                return "Troca qual botão do controle faz cada ação do PS2 (por exemplo, pular no B em vez do A). "
                       "Cada jogador tem o seu próprio mapa.";
            // Linhas originais desativadas (a tela agora edita as teclas):
            // case Row::KeyboardKeys:
            //     return "Mostra as teclas do teclado e os atalhos do port (tela cheia, screenshot).";
            case Row::KeyboardKeys:
                return "Troca as teclas do teclado e os botões do mouse de cada ação (até 4 por ação), e liga ou "
                       "desliga o mouse no jogo. Padrão: clique esquerdo ataca, direito faz o ataque carregado.";
            case Row::Play:
                return "Salva as opções e abre o jogo.";
            case Row::Save:
                return "Salva as opções e fecha. Depois é só abrir WarriorsOrochi.exe.";
            case Row::Quit:
                return "Fecha sem salvar as mudanças.";
            default:
                return "";
            }
        }

        void change(Row row, int direction)
        {
            if (!enabled(row))
                return;
            Settings &s = m_settings;
            auto step = [direction](int value, int count) { return (value + direction + count) % count; };
            switch (row)
            {
            case Row::Disc:
                m_disc = static_cast<size_t>(step(static_cast<int>(m_disc), static_cast<int>(m_discs.size())));
                s.disc = orochiUtf8(m_discs[m_disc]);
                break;
            case Row::Browse:
                browse();
                break;
            case Row::AutoSave:
                s.autoSave = !s.autoSave;
                break;
            case Row::Renderer:
                s.pcsx2 = !s.pcsx2;
                s.windowSize = std::min<int>(s.windowSize, static_cast<int>(s.sizes().size()) - 1);
                break;
            case Row::Scale: {
                const auto at = std::find(kScales.begin(), kScales.end(), s.scale) - kScales.begin();
                s.scale = kScales[step(static_cast<int>(at), static_cast<int>(kScales.size()))];
                break;
            }
            case Row::Aspect:
                s.widescreen = !s.widescreen;
                break;
            case Row::Display:
                s.fullscreen = !s.fullscreen;
                break;
            case Row::WindowSize:
                s.windowSize = step(s.windowSize, static_cast<int>(s.sizes().size()));
                break;
            case Row::Deinterlace:
                s.deinterlace = !s.deinterlace;
                break;
            case Row::Fxaa:
                s.fxaa = !s.fxaa;
                break;
            case Row::SeamFix:
                s.seamFix = !s.seamFix;
                break;
            case Row::Crowd: {
                const int levels[] = {51, 100, 147};
                const int at = s.crowd <= 51 ? 0 : s.crowd >= 147 ? 2 : 1;
                s.crowd = levels[step(at, 3)];
                break;
            }
            case Row::Audio:
                s.audio = !s.audio;
                break;
            case Row::Volume:
                s.volume = std::clamp(s.volume + direction * 10, 0, 100);
                break;
            case Row::CheatInvincible:
                s.cheatInvincible = !s.cheatInvincible;
                break;
            case Row::CheatLife:
                s.cheatLife = !s.cheatLife;
                break;
            case Row::CheatMusou:
                s.cheatMusou = !s.cheatMusou;
                break;
            case Row::CheatPoints:
                s.cheatPoints = !s.cheatPoints;
                break;
            case Row::PadPlayer1:
            case Row::PadPlayer2: {
                // Passa para a opcao anterior/seguinte de controllerChoicesFor.
                const int player = row == Row::PadPlayer1 ? 0 : 1;
                const std::vector<std::string> choices = controllerChoicesFor(player);
                const int at = static_cast<int>(std::find(choices.begin(), choices.end(), s.padPlayer[player]) - choices.begin());
                s.padPlayer[player] = choices[step(at, static_cast<int>(choices.size()))];
                break;
            }
            case Row::KeyboardPlayer: {
                // Ordem: jogador 1 -> jogador 2 -> ninguem -> jogador 1.
                const int order[] = {1, 2, 0};
                const int at = s.keyboardPlayer == 2 ? 1 : s.keyboardPlayer == 0 ? 2 : 0;
                s.keyboardPlayer = order[step(at, 3)];
                break;
            }
            case Row::ButtonsPlayer1:
                openSubPage(Page::ButtonsPlayer1);
                break;
            case Row::ButtonsPlayer2:
                openSubPage(Page::ButtonsPlayer2);
                break;
            case Row::KeyboardKeys:
                openSubPage(Page::KeyboardKeys);
                break;
            default:
                break;
            }
        }

        void browse()
        {
            std::filesystem::path disc;
            if (!orochiPickDisc(GetWindowHandle(), disc))
                return;
            if (!orochiIsCompatibleDisc(disc))
            {
                m_problem = "Esta ISO não é compatível: o port só abre o Warriors Orochi (USA) e versões "
                            "dele que mudam apenas textos, como a tradução.";
                return;
            }
            m_problem.clear();
            m_discs = orochiCompatibleDiscs(disc.parent_path());
            auto at = std::find(m_discs.begin(), m_discs.end(), disc);
            if (at == m_discs.end()) // a compatible image without the .iso extension
                at = m_discs.insert(m_discs.end(), disc);
            m_disc = static_cast<size_t>(at - m_discs.begin());
            m_settings.disc = orochiUtf8(disc);
            m_focus = Row::Play;
        }

        void moveFocus(int direction)
        {
            const std::vector<Row> rows = visible();
            const int count = static_cast<int>(rows.size());
            int at = static_cast<int>(std::find(rows.begin(), rows.end(), m_focus) - rows.begin());
            if (at >= count)
                at = direction > 0 ? -1 : 0;
            for (int tries = 0; tries < count; ++tries)
            {
                at = (at + direction + count) % count;
                if (enabled(rows[at]))
                {
                    m_focus = rows[at];
                    return;
                }
            }
        }

        void showTab(Tab tab)
        {
            m_tab = tab;
            m_problem.clear();
            // The first usable row of the tab, or Play on a tab without options.
            for (Row row : visible())
                if (!isButton(row) && enabled(row))
                {
                    m_focus = row;
                    return;
                }
            m_focus = enabled(Row::Play) ? Row::Play : Row::Save;
        }

        void nextTab(int direction)
        {
            const int count = static_cast<int>(Tab::Count);
            showTab(static_cast<Tab>((static_cast<int>(m_tab) + direction + count) % count));
        }

        // Layout, in the 960xkWindowHeight setup window (scaled if it is resized).
        float scale() const { return std::min(GetScreenWidth() / 960.0f, GetScreenHeight() / static_cast<float>(kWindowHeight)); }
        Rectangle tabRect(Tab tab) const
        {
            const float k = scale(), width = 860.0f / static_cast<float>(Tab::Count);
            return {k * (50 + width * static_cast<float>(tab)), k * 122, k * (width - 4), k * 46};
        }
        Rectangle rowRect(Row row) const
        {
            const float k = scale();
            if (row == Row::Play)
                return {k * 190, k * 664, k * 240, k * 52};
            if (row == Row::Save)
                return {k * 450, k * 664, k * 170, k * 52};
            if (row == Row::Quit)
                return {k * 640, k * 664, k * 130, k * 52};
            int index = 0;
            for (Row other : visible())
            {
                if (other == row)
                    break;
                ++index;
            }
            return {k * 60, k * (kRowsTop + index * kRowPitch), k * 840, k * 42};
        }
        Rectangle valueRect(Row row) const
        {
            const Rectangle r = rowRect(row);
            const float k = scale();
            return {r.x + k * 400, r.y, r.width - k * 400, r.height};
        }

        bool finish(Row row, Outcome &outcome)
        {
            outcome = row == Row::Play ? Outcome::Play : row == Row::Save ? Outcome::Save : Outcome::Quit;
            return true;
        }

        // Returns true when the screen is done, with the choice in `outcome`.
        bool input(Outcome &outcome)
        {
            // Telas extras (botoes / teclas) tem a sua propria entrada.
            if (m_page != Page::Main)
            {
                inputSubPage();
                return false;
            }
            const bool pad = IsGamepadAvailable(0);
            auto pressed = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
            auto padPressed = [pad](int button) { return pad && IsGamepadButtonPressed(0, button); };
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            if ((IsKeyPressed(KEY_TAB) && !shift) || IsKeyPressed(KEY_PAGE_DOWN) || IsKeyPressed(KEY_E) ||
                padPressed(GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
                nextTab(1);
            if ((IsKeyPressed(KEY_TAB) && shift) || IsKeyPressed(KEY_PAGE_UP) || IsKeyPressed(KEY_Q) ||
                padPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_1))
                nextTab(-1);
            if (pressed(KEY_DOWN) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                moveFocus(1);
            if (pressed(KEY_UP) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP))
                moveFocus(-1);
            if (pressed(KEY_LEFT) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                isButton(m_focus) ? moveFocus(-1) : change(m_focus, -1);
            if (pressed(KEY_RIGHT) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
                isButton(m_focus) ? moveFocus(1) : change(m_focus, 1);
            if (IsKeyPressed(KEY_SPACE) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            {
                if (isButton(m_focus))
                    return finish(m_focus, outcome);
                change(m_focus, 1);
            }
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || padPressed(GAMEPAD_BUTTON_MIDDLE_RIGHT))
            {
                // Linhas que abrem telas extras: Enter abre a tela (sem isso,
                // Enter numa linha comum inicia o jogo, como antes).
                if (m_focus == Row::ButtonsPlayer1 || m_focus == Row::ButtonsPlayer2 || m_focus == Row::KeyboardKeys)
                    change(m_focus, 1);
                else
                if (m_focus == Row::Browse)
                    change(m_focus, 1);
                else if (isButton(m_focus))
                    return finish(m_focus, outcome);
                else if (enabled(Row::Play))
                    return finish(Row::Play, outcome);
            }

            const Vector2 mouse = GetMousePosition();
            const Vector2 delta = GetMouseDelta();
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                for (int index = 0; index < static_cast<int>(Tab::Count); ++index)
                    if (CheckCollisionPointRec(mouse, tabRect(static_cast<Tab>(index))))
                        showTab(static_cast<Tab>(index));
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f && CheckCollisionPointRec(mouse, {0, 0, static_cast<float>(GetScreenWidth()), tabRect(Tab::Game).y + tabRect(Tab::Game).height}))
                nextTab(wheel < 0 ? 1 : -1);
            for (Row row : visible())
            {
                if (!enabled(row) || !CheckCollisionPointRec(mouse, rowRect(row)))
                    continue;
                if (delta.x != 0.0f || delta.y != 0.0f)
                    m_focus = row;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
                {
                    if (isButton(row))
                        return finish(row, outcome);
                    const Rectangle v = valueRect(row);
                    change(row, mouse.x < v.x + v.width / 2 ? -1 : 1);
                }
            }
            return false;
        }

        void text(const Font &font, const std::string &string, float x, float y, float size, Color color) const
        {
            DrawTextEx(font, string.c_str(), {x, y}, size, 0.0f, color);
        }
        // Word-wrapped text from (x, y), at most `width` wide.
        void wrapped(const Font &font, const std::string &string, float x, float y, float width, float size,
                     Color color) const
        {
            std::string line, word;
            auto flush = [&]() {
                text(font, line, x, y, size, color);
                y += size * 1.15f;
                line.clear();
            };
            for (size_t at = 0; at <= string.size(); ++at)
            {
                if (at < string.size() && string[at] != ' ')
                {
                    word += string[at];
                    continue;
                }
                const std::string candidate = line.empty() ? word : line + ' ' + word;
                if (!line.empty() && MeasureTextEx(font, candidate.c_str(), size, 0.0f).x > width)
                {
                    flush();
                    line = word;
                }
                else
                    line = candidate;
                word.clear();
            }
            if (!line.empty())
                flush();
        }
        void centered(const Font &font, const std::string &string, Rectangle box, float size, Color color) const
        {
            const Vector2 extent = MeasureTextEx(font, string.c_str(), size, 0.0f);
            text(font, string, box.x + (box.width - extent.x) / 2, box.y + (box.height - extent.y) / 2, size, color);
        }

        void draw() const
        {
            // Telas extras (botoes / teclas) se desenham sozinhas.
            if (m_page != Page::Main)
            {
                drawSubPage();
                return;
            }
            const float k = scale();
            const Color gold{226, 186, 92, 255}, ink{232, 234, 240, 255}, dim{120, 126, 140, 255},
                focus{60, 82, 130, 255}, panel{20, 26, 40, 230}, tabIdle{30, 36, 54, 230};
            BeginDrawing();
            DrawRectangleGradientV(0, 0, GetScreenWidth(), GetScreenHeight(), Color{14, 18, 30, 255}, Color{40, 22, 30, 255});
            text(m_fonts.title, "WARRIORS OROCHI", k * 60, k * 30, k * 44, gold);
            text(m_fonts.small, "Setup do port para PC", k * 62, k * 80, k * 20, dim);

            for (int index = 0; index < static_cast<int>(Tab::Count); ++index)
            {
                const Tab tab = static_cast<Tab>(index);
                const Rectangle r = tabRect(tab);
                const bool current = tab == m_tab;
                DrawRectangleRec(r, current ? panel : tabIdle);
                if (current)
                    DrawRectangleRec({r.x, r.y, r.width, k * 3}, gold);
                centered(m_fonts.text, tabName(tab), r, k * 24, current ? gold : dim);
            }
            DrawRectangleRec({k * 50, k * 168, k * 860, k * (kPanelBottom - 168)}, panel);

            // Bloco original desativado: a lista de teclas ocupava a aba
            // Controles inteira; agora a aba tem opcoes e a lista foi para a
            // tela "Teclas do teclado" (drawSubPage).
            // if (m_tab == Tab::Controls)
            // {
            //     float y = kRowsTop - 6;
            //     for (const KeyLine &line : kKeys)
            //     {
            //         text(m_fonts.small, line.action, k * 76, k * y, k * 20, ink);
            //         text(m_fonts.small, line.keys, k * 460, k * y, k * 20, gold);
            //         y += 29;
            //     }
            // }
            if (m_tab == Tab::Controls)
                drawConnectedControllersList(k);

            for (Row row : visible())
            {
                const Rectangle r = rowRect(row);
                const bool on = enabled(row), focused = row == m_focus;
                if (isButton(row))
                {
                    const bool play = row == Row::Play;
                    DrawRectangleRounded(r, 0.25f, 6, !on ? Color{44, 50, 66, 255}
                                                      : play ? (focused ? gold : Color{150, 120, 60, 255})
                                                             : (focused ? focus : Color{44, 50, 66, 255}));
                    centered(m_fonts.text, play ? "Salvar e jogar" : row == Row::Save ? "Salvar" : "Sair", r, k * 26,
                             !on ? dim : play ? Color{24, 18, 10, 255} : ink);
                    continue;
                }
                if (focused)
                    DrawRectangleRounded(r, 0.2f, 6, focus);
                const Color color = on ? ink : dim;
                text(m_fonts.text, label(row), r.x + k * 16, r.y + k * 7, k * 26, color);
                const Rectangle v = valueRect(row);
                const std::string shown = value(row);
                const float room = v.width - k * 48, wide = MeasureTextEx(m_fonts.text, shown.c_str(), k * 24, 0.0f).x;
                centered(m_fonts.text, shown, v, k * 24 * std::min(1.0f, room / wide), on ? (focused ? gold : ink) : dim);
                if (on && focused && row != Row::Browse)
                {
                    const float cy = v.y + v.height / 2, s = k * 8;
                    DrawTriangle({v.x + k * 6, cy}, {v.x + k * 6 + s, cy + s}, {v.x + k * 6 + s, cy - s}, gold);
                    DrawTriangle({v.x + v.width - k * 6, cy}, {v.x + v.width - k * 6 - s, cy - s},
                                 {v.x + v.width - k * 6 - s, cy + s}, gold);
                }
            }

            const char *about = m_tab == Tab::Controls && isButton(m_focus)
                                    ? "Controles de jogo conectados ao Windows também funcionam. Com a janela "
                                      "do jogo fora de foco, o controle é ignorado."
                                    : help(m_focus);
            if (m_problem.empty())
                wrapped(m_fonts.small, about, k * 60, k * 572, k * 840, k * 20, ink);
            else
                wrapped(m_fonts.small, m_problem, k * 60, k * 572, k * 840, k * 20, Color{240, 120, 110, 255});
            text(m_fonts.small, "Q/E ou Tab trocar de aba    ↑↓ escolher    ←→ alterar    Enter confirmar    Esc sair",
                 k * 60, k * 730, k * 18, dim);
            EndDrawing();
        }
    };
}

int main()
{
    SetTraceLogLevel(LOG_NONE);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(960, kWindowHeight, "Warriors Orochi - Setup");
    SetTargetFPS(60);
    orochiRestoreWindow(GetWindowHandle());
    // Os mesmos mapeamentos extras de controle que o jogo usa, para que a
    // lista de controles do Setup seja igual a do jogo.
    loadGamepadMappingsLikeTheGame();
    Settings settings = orochiLoadSettings();
    std::vector<std::filesystem::path> discs;
    size_t chosen = 0;
    if (!settings.disc.empty())
    {
        const auto disc = orochiPathFromUtf8(settings.disc);
        discs = orochiCompatibleDiscs(disc.parent_path());
        const auto at = std::find(discs.begin(), discs.end(), disc);
        if (at != discs.end())
            chosen = static_cast<size_t>(at - discs.begin());
        else
            settings.disc.clear();
    }
    const Outcome outcome = Screen(settings, discs, chosen).run();
    CloseWindow();
    if (outcome != Outcome::Quit)
        orochiSaveSettings(settings);
    if (outcome == Outcome::Play)
        orochiRunProgram(kOrochiGameProgram);
    return 0;
}
