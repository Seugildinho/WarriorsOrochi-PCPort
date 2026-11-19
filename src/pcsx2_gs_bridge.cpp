// Runs PCSX2's GS for the port: GIF packets and VBlanks from the runtime's GS
// frontend (its packet and VBlank taps) are queued to a thread that owns the
// GS and its Direct3D 11 device. Compiled into pcsx2_gs.
// ==========================================================================
// PONTE ENTRE O JOGO E O GS DO PCSX2 (o "processador grafico" do PS2)
// ==========================================================================
// O que este arquivo faz, em ordem:
//   1. O jogo recompilado manda pacotes GIF (comandos de desenho do PS2:
//      triangulos, sprites, texturas) e, a cada quadro, um VBlank (o sinal
//      "terminei o quadro, pode mostrar").
//   2. Packet() e VSync() (no fim do arquivo) apenas COLOCAM esses comandos
//      numa fila (s_queue). Elas rodam na thread do jogo e retornam rapido.
//   3. Uma thread propria (funcao run()) e a UNICA dona do GS do PCSX2 e do
//      dispositivo Direct3D 11. Ela tira os comandos da fila e os executa:
//        - Packet -> GSgifTransfer (desenha)
//        - VSync  -> GSvsync (mostra o quadro na janela)
// Por que uma thread so para o GS: o Direct3D 11 do PCSX2 nao pode ser usado
// de varias threads ao mesmo tempo, e assim o jogo nao espera a placa de video.
//
// Resolucao e qualidade de imagem: sao definidas em run() (UpscaleMultiplier,
// AspectRatio 4:3/16:9, filtros) e podem ser ajustadas pela variavel de
// ambiente OROCHI_PCSX2_FIXES (veja applyFixes()).
#include "pcsx2_host.h"

#include "Config.h"
#include "common/Console.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSRenderer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Chave liga/desliga da correcao de bordas de sprites, definida no PCSX2
// (thirdparty/pcsx2/.../GSRendererHW.cpp, patch patches/pcsx2/0002).
// Quando true, o PCSX2 "encolhe" meio texel as coordenadas de textura dos
// sprites filtrados, para que o upscale nao puxe pixels vizinhos da textura
// (isso causava linhas finas no HUD). Ligada por "edge=1" em applyFixes().
extern bool g_orochi_sprite_edge_inset; // GSRendererHW.cpp (sprite edge correction)

namespace
{
    // Um comando na fila da thread do GS. "kind" diz qual dos campos vale:
    //   Packet     -> data (bytes GIF, sempre multiplos de 16 = quadwords)
    //   VSync      -> registers + odd (fim de quadro; odd = campo impar)
    //   Resize     -> width/height (a janela mudou de tamanho)
    //   DumpMemory -> nenhum (salva a VRAM do GS para diagnostico)
    //   Stop       -> nenhum (fecha o GS e encerra a thread)
    struct Command
    {
        enum class Kind
        {
            Packet,
            VSync,
            Resize,
            DumpMemory,
            Stop,
        } kind = Kind::Packet;
        std::vector<u8> data;
        OrochiPcsx2Gs::Registers registers{};
        bool odd = false;
        unsigned width = 0, height = 0;
    };

    // Estado compartilhado entre a thread do jogo e a thread do GS.
    // Tudo abaixo so e lido/escrito com s_mutex travado.
    //   s_wake          : acorda a thread do GS quando entra comando na fila.
    //   s_progress      : avisa a thread do jogo que um VSync foi consumido
    //                     (usado para limitar quantos quadros ficam pendentes).
    //   s_partial       : sobra de bytes que ainda nao fecha 16 bytes; o GS so
    //                     aceita quadwords inteiras, entao guardamos ate completar.
    //   s_vsyncsQueued  : quantos VSync estao na fila ainda nao mostrados.
    //   s_ready/s_open  : a thread do GS terminou de iniciar / conseguiu abrir.
    //   s_privileged    : copia dos registradores privilegiados do GS (PMODE,
    //                     DISPFB, DISPLAY...) que o PCSX2 le para montar a imagem.
    std::mutex s_mutex;
    std::condition_variable s_wake, s_progress;
    std::deque<Command> s_queue;
    std::vector<u8> s_partial;  // bytes of an incomplete quadword
    unsigned s_vsyncsQueued = 0;
    bool s_ready = false, s_open = false;
    std::thread s_thread;
    alignas(16) u8 s_privileged[0x2000];

    // Frame capture for diagnostics (OROCHI_GIF_CAPTURE=<file>): once
    // <file>.trigger exists or F9 is pressed, the GIF bytes of the next
    // VBlank intervals (OROCHI_GIF_CAPTURE_VSYNCS, default 2) are written to <file> ("OGIF", then per
    // VBlank its registers and the size and bytes of what came before it), and
    // the GS memory as it stands to <file>.vram.
    const unsigned kCaptureVSyncs = std::getenv("OROCHI_GIF_CAPTURE_VSYNCS")
                                        ? static_cast<unsigned>(std::atoi(std::getenv("OROCHI_GIF_CAPTURE_VSYNCS")))
                                        : 2u;
    const char *const s_capturePath = std::getenv("OROCHI_GIF_CAPTURE");
    unsigned s_captureLeft = 0;
    std::atomic<bool> s_captureRequested{false};
    std::vector<u8> s_capture, s_captureFile;

    // captureVSync: parte da captura de quadros para diagnostico (so funciona se
    // OROCHI_GIF_CAPTURE estiver definida; no jogo normal retorna na 1a linha).
    // Chamada a cada VBlank:
    //   - Se uma captura esta em andamento (s_captureLeft > 0), grava no buffer os
    //     registradores deste VBlank + o tamanho + os bytes GIF recebidos desde o
    //     VBlank anterior. Ao terminar o ultimo VBlank, escreve o arquivo e pede
    //     um DumpMemory (VRAM).
    //   - Senao, verifica se pediram uma captura (tecla F9 ou arquivo
    //     "<arquivo>.trigger") e, se sim, comeca uma nova.
    // Exemplo: OROCHI_GIF_CAPTURE=C:\WO\cap.ogif e criar cap.ogif.trigger gera
    // cap.ogif (2 quadros) e cap.ogif.vram; o tools/pcsx2_gs_replay.exe reproduz.
    void captureVSync(const OrochiPcsx2Gs::Registers &registers)
    {
        if (!s_capturePath)
            return;
        if (s_captureLeft > 0)
        {
            const u32 size = static_cast<u32>(s_capture.size());
            const u8 *fields = reinterpret_cast<const u8 *>(&registers);
            s_captureFile.insert(s_captureFile.end(), fields, fields + sizeof(registers));
            s_captureFile.insert(s_captureFile.end(), reinterpret_cast<const u8 *>(&size),
                                 reinterpret_cast<const u8 *>(&size) + sizeof(size));
            s_captureFile.insert(s_captureFile.end(), s_capture.begin(), s_capture.end());
            s_capture.clear();
            if (--s_captureLeft == 0)
            {
                if (std::FILE *file = std::fopen(s_capturePath, "wb"))
                {
                    std::fwrite(s_captureFile.data(), 1, s_captureFile.size(), file);
                    std::fclose(file);
                    std::fprintf(stderr, "[gif-capture] %zu bytes to %s\n", s_captureFile.size(), s_capturePath);
                }
                s_captureFile.clear();
                Command dump;
                dump.kind = Command::Kind::DumpMemory;
                s_queue.push_back(std::move(dump));
            }
            return;
        }
        const std::string trigger = std::string(s_capturePath) + ".trigger";
        if (s_captureRequested.exchange(false) || std::remove(trigger.c_str()) == 0)
        {
            s_captureLeft = kCaptureVSyncs;
            s_captureFile.assign({'O', 'G', 'I', 'F'});
        }
    }

    // dumpMemory: grava os 4 MB da memoria do GS (VRAM) em "<captura>.vram".
    // Roda na thread do GS (comando DumpMemory) porque so ela acessa o renderer.
    void dumpMemory()
    {
        const std::string path = std::string(s_capturePath) + ".vram";
        if (std::FILE *file = std::fopen(path.c_str(), "wb"))
        {
            std::fwrite(g_gs_renderer->m_mem.vm8(), 1, 4u * 1024u * 1024u, file);
            std::fclose(file);
        }
    }

    // writeRegisters: copia os registradores de video que o jogo programou
    // (modo de exibicao, endereco do framebuffer, posicao/tamanho da imagem, cor
    // de fundo) para s_privileged, nos mesmos deslocamentos do hardware do PS2
    // (ex.: PMODE em 0x00, DISPFB1 em 0x70, DISPLAY1 em 0x80). O PCSX2 le esse
    // bloco no GSvsync para saber o que e onde mostrar.
    // "odd" vira o bit FIELD do CSR: indica se o campo exibido e o impar.
    void writeRegisters(const OrochiPcsx2Gs::Registers &r, bool odd)
    {
        auto put = [](u32 offset, u64 value) { std::memcpy(s_privileged + offset, &value, sizeof(value)); };
        put(0x00, r.pmode);
        put(0x10, r.smode1);
        put(0x20, r.smode2);
        put(0x30, r.srfsh);
        put(0x40, r.synch1);
        put(0x50, r.synch2);
        put(0x60, r.syncv);
        put(0x70, r.dispfb1);
        put(0x80, r.display1);
        put(0x90, r.dispfb2);
        put(0xA0, r.display2);
        put(0xE0, r.bgcolor);
        // CSR.FIELD (bit 13): the field being displayed.
        put(0x1000, (r.csr & ~(1ull << 13)) | (odd ? 1ull << 13 : 0ull));
    }

    // PCSX2's upscaling corrections from "key=value,..." (OROCHI_PCSX2_FIXES):
    // hpo (GSHalfPixelOffset), round (round sprite 0..2), align, merge, even
    // (align/merge/force-even sprites 0/1), native (GSNativeScaling), tcx, tcy
    // (texture offsets), deinterlace (GSInterlaceMode), fxaa (0/1), scale,
    // aniso (max anisotropy), blend (AccBlendLevel), trilinear (TriFiltering),
    // present (GSPostBilinearMode), cas (0/1, sharpening), sharpness (0..100),
    // preload (TexturePreloadingLevel), mipmap (0/1).
    // applyFixes: le a lista "chave=valor,chave=valor" de OROCHI_PCSX2_FIXES e
    // ajusta as opcoes do GS do PCSX2. Chaves desconhecidas sao ignoradas e
    // itens sem "=" sao pulados. Qualquer chave ativa ManualUserHacks (sem isso
    // o PCSX2 ignora os ajustes manuais).
    // Exemplo real usado pelo Setup ("Correcao de linhas" ligada):
    //   OROCHI_PCSX2_FIXES=...,edge=1,hpo=5
    //   -> correcao de borda de sprites ligada + modo 5 de meio-pixel.
    // Exemplo para testar resolucao: "scale=4" = 4x a resolucao interna do PS2.
    // Os valores sao limitados (std::clamp) para nunca sair da faixa valida.
    void applyFixes(Pcsx2Config::GSOptions &options, const std::string &spec)
    {
        size_t at = 0;
        while (at < spec.size())
        {
            const size_t end = std::min(spec.find(',', at), spec.size());
            const std::string item = spec.substr(at, end - at);
            at = end + 1;
            const size_t equals = item.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = item.substr(0, equals);
            const int value = std::atoi(item.c_str() + equals + 1);
            options.ManualUserHacks = true;
            if (key == "hpo")
                options.UserHacks_HalfPixelOffset = static_cast<GSHalfPixelOffset>(
                    std::clamp(value, 0, static_cast<int>(GSHalfPixelOffset::MaxCount) - 1));
            else if (key == "round")
                options.UserHacks_RoundSprite = static_cast<s8>(std::clamp(value, 0, 2));
            else if (key == "align")
                options.UserHacks_AlignSpriteX = value != 0;
            else if (key == "merge")
                options.UserHacks_MergePPSprite = value != 0;
            else if (key == "even")
                options.UserHacks_ForceEvenSpritePosition = value != 0;
            else if (key == "native")
                options.UserHacks_NativeScaling = static_cast<GSNativeScaling>(std::clamp(value, 0, 4));
            else if (key == "tcx")
                options.UserHacks_TCOffsetX = value;
            else if (key == "tcy")
                options.UserHacks_TCOffsetY = value;
            else if (key == "deinterlace")
                options.InterlaceMode = static_cast<GSInterlaceMode>(
                    std::clamp(value, 0, static_cast<int>(GSInterlaceMode::Count) - 1));
            else if (key == "fxaa")
                options.FXAA = value != 0;
            else if (key == "scale")
                options.UpscaleMultiplier = static_cast<float>(std::clamp(value, 1, 8));
            else if (key == "aniso")
                options.MaxAnisotropy = static_cast<u8>(std::clamp(value, 0, 16));
            else if (key == "blend")
                options.AccurateBlendingUnit = static_cast<AccBlendLevel>(
                    std::clamp(value, 0, static_cast<int>(AccBlendLevel::MaxCount) - 1));
            else if (key == "trilinear")
                options.TriFilter = static_cast<TriFiltering>(std::clamp(value, -1, 2));
            else if (key == "present")
                options.LinearPresent = static_cast<GSPostBilinearMode>(std::clamp(value, 0, 2));
            else if (key == "cas")
                options.CASMode = value != 0 ? GSCASMode::SharpenOnly : GSCASMode::Disabled;
            else if (key == "sharpness")
                options.CAS_Sharpness = static_cast<u8>(std::clamp(value, 0, 100));
            else if (key == "preload")
                options.TexturePreloading = static_cast<TexturePreloadingLevel>(std::clamp(value, 0, 2));
            else if (key == "mipmap")
                options.HWMipmap = value != 0;
            else if (key == "edge")
                g_orochi_sprite_edge_inset = value != 0;
        }
    }

    // run: corpo da thread do GS. Faz tudo que envolve o PCSX2/Direct3D 11.
    // Parametros:
    //   resources  : pasta pcsx2-resources (shaders do PCSX2).
    //   upscale    : multiplicador de resolucao (1 = nativo 640x448, 2 = 2x...).
    //   widescreen : true mostra em 16:9, false em 4:3.
    // Passos:
    //   1. Liga o log do PCSX2 no stderr (vai para data\port.log).
    //   2. Define a pasta "shader-cache" (shaders compilados ficam salvos e o jogo
    //      nao trava na 2a vez que desenha um efeito).
    //   3. Monta as opcoes de imagem padrao do port e aplica OROCHI_PCSX2_FIXES.
    //   4. Abre o GS (GSopen). Avisa Start() se deu certo (s_ready/s_open).
    //   5. Laco infinito: pega TODOS os comandos da fila de uma vez (batch) e
    //      executa um por um, ate receber Stop.
    void run(std::string resources, unsigned upscale, bool widescreen)
    {
        // PCSX2's log (device and shader errors) on stderr.
        Log::SetHostOutputLevel(LOGLEVEL_INFO, [](LOGLEVEL, ConsoleColors, std::string_view message) {
            std::fprintf(stderr, "[pcsx2] %.*s\n", static_cast<int>(message.size()), message.data());
        });
        EmuFolders::Resources = std::move(resources);
        // Compiled shaders are kept beside the saves; without a folder each
        // one is compiled again the first time a run draws with it.
        std::error_code error;
        const auto cache = std::filesystem::current_path(error) / "shader-cache";
        std::filesystem::create_directories(cache, error);
        if (!error)
            EmuFolders::Cache = cache.string();
        Pcsx2Config::GSOptions options;
        options.Renderer = GSRendererType::DX11;
        options.UpscaleMultiplier = static_cast<float>(upscale);
        options.AspectRatio = widescreen ? AspectRatioType::R16_9 : AspectRatioType::R4_3;
        // The port's picture: the field offset is patched out of the game
        // (fixedFieldOffset), so nothing needs PCSX2's field blending, and the
        // defaults below sharpen textures at a distance and the final scaling.
        // Padroes de imagem do port (podem ser trocados por OROCHI_PCSX2_FIXES):
        //   InterlaceMode Off  : sem mistura de campos (o jogo ja foi corrigido).
        //   MaxAnisotropy 16   : texturas no chao/longe ficam nitidas.
        //   Medium blending    : mistura de transparencias mais fiel ao PS2.
        //   BilinearSharp      : escala final da imagem para a janela, suave e nitida.
        options.InterlaceMode = GSInterlaceMode::Off;
        options.MaxAnisotropy = 16;
        options.AccurateBlendingUnit = AccBlendLevel::Medium;
        options.LinearPresent = GSPostBilinearMode::BilinearSharp;
        if (const char *fixes = std::getenv("OROCHI_PCSX2_FIXES"))
            applyFixes(options, fixes);
        // The display reads the current ratio (PCSX2's VM sets it per FMV).
        EmuConfig.GS.AspectRatio = options.AspectRatio;
        EmuConfig.CurrentAspectRatio = options.AspectRatio;
        const bool open = GSopen(options, GSRendererType::DX11, s_privileged, GSVSyncMode::Disabled, false);
        {
            std::lock_guard lock(s_mutex);
            s_ready = true;
            s_open = open;
        }
        s_progress.notify_all();
        if (!open)
            return;
        std::deque<Command> batch;
        for (;;)
        {
            {
                std::unique_lock lock(s_mutex);
                s_wake.wait(lock, [] { return !s_queue.empty(); });
                batch.swap(s_queue);
            }
            for (Command &command : batch)
            {
                switch (command.kind)
                {
                case Command::Kind::Packet:
                    GSgifTransfer(command.data.data(), static_cast<u32>(command.data.size() / 16u));
                    break;
                case Command::Kind::VSync:
                    writeRegisters(command.registers, command.odd);
                    GSvsync(command.odd ? 1u : 0u, true);
                    {
                        std::lock_guard lock(s_mutex);
                        --s_vsyncsQueued;
                    }
                    s_progress.notify_all();
                    break;
                case Command::Kind::Resize:
                    OrochiPcsx2Host::SetRenderWindow(nullptr, command.width, command.height);
                    GSResizeDisplayWindow(command.width, command.height, 1.0f);
                    break;
                case Command::Kind::DumpMemory:
                    dumpMemory();
                    break;
                case Command::Kind::Stop:
                    GSclose();
                    return;
                }
            }
            batch.clear();
        }
    }

    // push: coloca um comando na fila e acorda a thread do GS. Usado pelos
    // comandos que nao precisam de logica extra (Resize, Stop).
    void push(Command command)
    {
        {
            std::lock_guard lock(s_mutex);
            s_queue.push_back(std::move(command));
        }
        s_wake.notify_one();
    }
}

// Start: inicia o GS do PCSX2. Chamado uma vez ao abrir o jogo.
//   1. Informa a janela (hwnd) e o tamanho da area de desenho.
//   2. Cria a thread run() e ESPERA ela terminar de abrir o GS.
//   3. Retorna false se o GS nao abriu (ex.: placa sem Direct3D 11); nesse
//      caso a thread ja terminou e foi unida (join).
bool OrochiPcsx2Gs::Start(const char *resources, void *hwnd, unsigned width, unsigned height, unsigned upscale,
                          bool widescreen)
{
    OrochiPcsx2Host::SetRenderWindow(hwnd, width, height);
    s_thread = std::thread(run, std::string(resources), upscale, widescreen);
    std::unique_lock lock(s_mutex);
    s_progress.wait(lock, [] { return s_ready; });
    if (!s_open)
    {
        lock.unlock();
        s_thread.join();
        return false;
    }
    return true;
}

// Packet: recebe bytes GIF do jogo, em qualquer tamanho.
// O GS so aceita quadwords inteiras (16 bytes), entao:
//   - junta os bytes novos em s_partial;
//   - envia para a fila so a parte multipla de 16;
//   - guarda o resto para o proximo Packet.
// Exemplo: chegam 40 bytes -> 32 vao para a fila, 8 ficam em s_partial.
// Se uma captura de diagnostico estiver ativa, os bytes tambem sao copiados.
void OrochiPcsx2Gs::Packet(const uint8_t *data, uint32_t size)
{
    Command command;
    {
        std::lock_guard lock(s_mutex);
        if (!s_open)
            return;
        if (s_captureLeft > 0)
            s_capture.insert(s_capture.end(), data, data + size);
        s_partial.insert(s_partial.end(), data, data + size);
        const size_t whole = s_partial.size() & ~size_t(15);
        if (whole == 0u)
            return;
        command.data.assign(s_partial.begin(), s_partial.begin() + static_cast<std::ptrdiff_t>(whole));
        s_partial.erase(s_partial.begin(), s_partial.begin() + static_cast<std::ptrdiff_t>(whole));
        s_queue.push_back(std::move(command));
    }
    s_wake.notify_one();
}

// VSync: fim de um quadro do jogo. Enfileira o comando que mostra a imagem.
// Controle de atraso: se ja ha 2 VSync esperando na fila, a thread do jogo
// FICA PARADA aqui ate o GS mostrar um deles. Assim o jogo nunca fica mais de
// 2 quadros a frente da imagem (sem isso a fila crescia sem limite: segundos
// de atraso nos controles e gigabytes de memoria).
void OrochiPcsx2Gs::VSync(const Registers &registers, bool oddField)
{
    {
        std::unique_lock lock(s_mutex);
        if (!s_open)
            return;
        s_progress.wait(lock, [] { return s_vsyncsQueued < 2u; });
        captureVSync(registers);
        Command command;
        command.kind = Command::Kind::VSync;
        command.registers = registers;
        command.odd = oddField;
        s_queue.push_back(std::move(command));
        ++s_vsyncsQueued;
    }
    s_wake.notify_one();
}

// Resize: a janela mudou de tamanho (ex.: F10 tela cheia). A thread do GS
// atualiza a area de desenho e redimensiona a imagem.
void OrochiPcsx2Gs::Resize(unsigned width, unsigned height)
{
    Command command;
    command.kind = Command::Kind::Resize;
    command.width = width;
    command.height = height;
    push(std::move(command));
}

// RequestCapture: pede uma captura de diagnostico no proximo VBlank (tecla F9).
// Retorna false se OROCHI_GIF_CAPTURE nao estiver definida.
bool OrochiPcsx2Gs::RequestCapture()
{
    if (!s_capturePath)
        return false;
    s_captureRequested = true;
    return true;
}

// Stop: fecha o GS ao sair do jogo. Envia Stop, espera a thread terminar e
// marca s_open=false para que Packet/VSync atrasados nao facam mais nada.
void OrochiPcsx2Gs::Stop()
{
    if (!s_thread.joinable())
        return;
    Command command;
    command.kind = Command::Kind::Stop;
    push(std::move(command));
    s_thread.join();
    std::lock_guard lock(s_mutex);
    s_open = false;
}
