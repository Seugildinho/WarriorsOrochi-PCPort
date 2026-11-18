// WarriorsOrochi-Setup.exe: chooses the disc and the options the game starts
// with, saved to orochi-settings.ini (src/orochi_settings.h). Drawn with raylib.
// The options are grouped in tabs (game, graphics, audio, controls, cheats).
// Compiled with /utf-8: the Portuguese strings below are UTF-8, as raylib's
// text functions expect.
#include "disc_image.h"
#include "orochi_settings.h"
#include "raylib.h"

#include <algorithm>
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
            default: return "";
            }
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

            if (m_tab == Tab::Controls)
            {
                float y = kRowsTop - 6;
                for (const KeyLine &line : kKeys)
                {
                    text(m_fonts.small, line.action, k * 76, k * y, k * 20, ink);
                    text(m_fonts.small, line.keys, k * 460, k * y, k * 20, gold);
                    y += 29;
                }
            }

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
