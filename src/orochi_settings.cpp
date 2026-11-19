// The settings file (src/orochi_settings.h).
// Leitura e gravacao do orochi-settings.ini (texto "chave=valor", uma por
// linha). Exemplo de arquivo:
//   renderer=pcsx2
//   scale=4
//   aspect=16:9
//   display=window
//   window=1280x720
//   seam_fix=on
//   crowd=100
// Valores invalidos ou ausentes ficam no padrao de OrochiSettings.
#include "orochi_settings.h"

#include "raylib.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace
{
    // Tamanhos de janela oferecidos no Setup, para 16:9 e para 4:3, e os
    // multiplicadores de resolucao interna (scale). Ex.: scale 3 = 1920x1344.
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

// orochiPadButtonMapToText: mapa -> "n,n,n,...". Ex.: padrao ->
// "1,3,4,2,7,6,8,5,9,11,10,12,15,13,16,17".
std::string orochiPadButtonMapToText(const OrochiPadButtonMap &map)
{
    std::string text;
    for (size_t index = 0; index < map.size(); ++index)
        text += (index ? "," : "") + std::to_string(map[index]);
    return text;
}

// orochiPadButtonMapFromText: "n,n,..." -> mapa. Precisa de exatamente 16
// numeros entre 0 e 17; qualquer outra coisa devolve o padrao inteiro (melhor
// que um controle com botoes faltando). Ex.: "7,6" -> padrao.
OrochiPadButtonMap orochiPadButtonMapFromText(const std::string &text)
{
    OrochiPadButtonMap map{};
    const char *at = text.c_str();
    for (size_t index = 0; index < map.size(); ++index)
    {
        char *end = nullptr;
        const long value = std::strtol(at, &end, 10);
        if (end == at || value < 0 || value > 17)
            return kOrochiDefaultPadButtonMap;
        map[index] = static_cast<int>(value);
        at = *end == ',' ? end + 1 : end;
    }
    return *at == '\0' ? map : kOrochiDefaultPadButtonMap;
}

// orochiKeyboardMapToText: mapa -> "a;b,c;...". Posicoes vazias (0) nao
// aparecem. Ex.: acao {88, 32, 75, 0} -> "88,32,75"; acao vazia -> "".
std::string orochiKeyboardMapToText(const OrochiKeyboardMap &map)
{
    std::string text;
    for (size_t action = 0; action < map.size(); ++action)
    {
        if (action)
            text += ';';
        bool first = true;
        for (const int code : map[action])
            if (code > 0)
            {
                text += (first ? "" : ",") + std::to_string(code);
                first = false;
            }
    }
    return text;
}

// Codigo aceito no mapa de teclado (mesma regra do ps2_pad.cpp).
static bool orochiIsValidKeyOrMouseCode(long code)
{
    return (code >= 1 && code <= 399) || (code >= kOrochiMouseButtonCodeBase && code <= kOrochiMouseButtonCodeBase + 6) ||
           code == kOrochiMouseWheelUpCode || code == kOrochiMouseWheelDownCode;
}

// orochiKeyboardMapFromText: texto -> mapa. Precisa de 20 acoes, cada uma
// com 0 a 4 codigos validos; qualquer erro devolve o padrao inteiro.
OrochiKeyboardMap orochiKeyboardMapFromText(const std::string &text)
{
    OrochiKeyboardMap map{};
    const char *at = text.c_str();
    for (int action = 0; action < kOrochiKeyboardActionCount; ++action)
    {
        int slot = 0;
        while (*at && *at != ';')
        {
            char *end = nullptr;
            const long code = std::strtol(at, &end, 10);
            if (end == at || !orochiIsValidKeyOrMouseCode(code) || slot >= kOrochiCodesPerKeyboardAction)
                return kOrochiDefaultKeyboardMap;
            map[action][slot++] = static_cast<int>(code);
            at = *end == ',' ? end + 1 : end;
        }
        const bool lastAction = action == kOrochiKeyboardActionCount - 1;
        if (lastAction ? *at != '\0' : *at != ';')
            return kOrochiDefaultKeyboardMap;
        if (!lastAction)
            ++at;
    }
    return map;
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

// orochiLoadSettings: le o ini. Regras de seguranca:
//   - crowd fica entre 51 e 147 (147 = limite da tabela de personagens).
//   - volume e arredondado para multiplos de 10, entre 0 e 100.
//   - scale fora da lista volta para 2.
//   - window so vale se for um dos tamanhos da lista do formato escolhido.
//   - disc so e aceito se for um caminho completo.
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
        // Controles. Ex. de linhas: "pad_player1=0:Xbox Controller",
        // "pad_player2=auto", "keyboard_player=1",
        // "pad_buttons_player1=1,3,4,2,7,6,8,5,9,11,10,12,15,13,16,17".
        // Valores invalidos: automatico / jogador 1 / mapa padrao.
        else if (key == "pad_player1" || key == "pad_player2")
            settings.padPlayer[key.back() == '1' ? 0 : 1] =
                (value == "none" || value.find(':') != std::string::npos) ? value : "auto";
        else if (key == "keyboard_player")
            settings.keyboardPlayer = value == "2" ? 2 : value == "0" ? 0 : 1;
        else if (key == "pad_buttons_player1" || key == "pad_buttons_player2")
            settings.padButtons[key.back() == '1' ? 0 : 1] = orochiPadButtonMapFromText(value);
        // Teclado e mouse. Ex.: "keyboard_map=265;264;...", "mouse_buttons=on".
        else if (key == "keyboard_map")
            settings.keyboardMap = orochiKeyboardMapFromText(value);
        else if (key == "mouse_buttons")
            settings.mouseButtons = value != "off";
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

// orochiSaveSettings: grava todas as opcoes (chamado pelo Setup ao clicar
// em "Salvar e jogar"). O arquivo e sempre reescrito inteiro.
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
         // Controles (formato descrito em orochiLoadSettings).
         << "pad_player1=" << settings.padPlayer[0] << '\n'
         << "pad_player2=" << settings.padPlayer[1] << '\n'
         << "keyboard_player=" << settings.keyboardPlayer << '\n'
         << "pad_buttons_player1=" << orochiPadButtonMapToText(settings.padButtons[0]) << '\n'
         << "pad_buttons_player2=" << orochiPadButtonMapToText(settings.padButtons[1]) << '\n'
         // Teclado e mouse (formato em orochi_settings.h, kOrochiKeyboardActionCount).
         << "keyboard_map=" << orochiKeyboardMapToText(settings.keyboardMap) << '\n'
         << "mouse_buttons=" << (settings.mouseButtons ? "on" : "off") << '\n'
         << "disc=" << settings.disc << '\n';
}
