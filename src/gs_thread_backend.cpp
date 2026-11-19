#include "gs_thread_backend.h"
// ==========================================================================
// THREAD DE DESENHO PARA OS RENDERIZADORES PROPRIOS (d3d11 e CPU)
// ==========================================================================
// Usado quando OROCHI_RENDERER e "auto" ou "d3d11" (NAO e usado com pcsx2,
// que tem a sua propria thread em pcsx2_gs_bridge.cpp).
// Ideia: em vez de o jogo esperar cada triangulo ser desenhado, os
// comandos de desenho entram numa fila e uma thread separada (worker)
// executa, NA MESMA ORDEM. O jogo so espera quando precisa de um resultado
// (ler a VRAM, limpar o framebuffer, etc.): isso e o "drain" (esvaziar a
// fila). Resultado: CPU do jogo e desenho trabalham em paralelo.
// Desligar: OROCHI_GS_THREAD=0 (main.cpp).
#include "runtime/gs/gs_cpu_backend.h"
#include <condition_variable>
#include <cstdlib>
#include <random>
#include <cstring>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {
// Tipos de comando que podem esperar na fila (os que nao devolvem nada).
// Command = um deles (std::variant).
struct LoadClutCommand { GSTex0Reg tex0; GSTexClutReg texclut; };
struct UploadCommand { std::vector<uint8_t> data; };
struct FlushCommand {};
struct TextureFlushCommand {};
struct WriteVramCommand { uint32_t psm, base, bw, x, y, value; };
struct SyncCommand { GSSyncReason reason; };
struct PresentCommand { GSPresentationRequest request; };
using Command = std::variant<GSPrimitiveBatch, LoadClutCommand, GSTransferCommand, UploadCommand,
                             FlushCommand, TextureFlushCommand, WriteVramCommand, SyncCommand, PresentCommand>;

// ThreadedBackend: "embrulha" um renderizador real (inner) e repassa a
// ele, pela fila, tudo que pode ser adiado.
// Funcoes que DEVOLVEM algo ao jogo (ReadVram, ClearFramebuffer,
// ConsumeLocalToHostBytes, SnapshotVram...) chamam drain() antes:
// esperam a fila esvaziar e so entao consultam o inner.
// OROCHI_ASYNC_PRESENT=0 deixa a apresentacao de quadro sincrona.
class ThreadedBackend final : public GSRasterBackend {
public:
    explicit ThreadedBackend(std::unique_ptr<GSRasterBackend> wrapped) : inner(std::move(wrapped)) {
        const char *async = std::getenv("OROCHI_ASYNC_PRESENT");
        asyncPresent = !async || std::string(async) != "0";
        worker = std::thread([this] { run(); });
    }
    // Destrutor: para a thread e imprime no log quantos comandos passaram e
    // quantas vezes/quanto tempo o jogo esperou em cada tipo de drain.
    // Ex.: "[Orochi:gs-thread] commands=20050 readVram=21/0ms".
    ~ThreadedBackend() override {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        worker.join();
        static constexpr const char *names[] = {"init", "reset", "transfer", "sync", "present", "clear",
                                                "localToHost", "readVram", "snapshot", "transferState"};
        std::cout << "[Orochi:gs-thread] commands=" << queued;
        for (size_t i = 0; i < drains.size(); ++i)
            if (drains[i]) std::cout << ' ' << names[i] << '=' << drains[i] << '/' << (waitMicroseconds[i] / 1000) << "ms";
        std::cout << '\n';
    }

    void Initialize(uint8_t *vram, uint32_t size) override { drain(0); std::lock_guard innerLock(innerMutex); inner->Initialize(vram, size); }
    void Reset() override { drain(1); std::lock_guard innerLock(innerMutex); inner->Reset(); }
    void Submit(const GSPrimitiveBatch &batch) override { push(std::in_place_type<GSPrimitiveBatch>, batch); }
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override { push(LoadClutCommand{tex0, texclut}); }
    void BeginTransfer(const GSTransferCommand &command) override {
        // A local-to-host transfer is read back by the caller; keep it synchronous.
        if (command.direction == 1u) { drain(2); std::lock_guard innerLock(innerMutex); inner->BeginTransfer(command); return; }
        push(command);
    }
    void UploadImage(const uint8_t *data, uint32_t size) override {
        push(UploadCommand{std::vector<uint8_t>(data, data + size)});
    }
    void Flush() override { push(FlushCommand{}); }
    void TextureFlush() override { push(TextureFlushCommand{}); }
    void Sync(GSSyncReason reason) override {
        // Presentation only orders the following Present, which is queued too.
        if (asyncPresent && reason == GSSyncReason::Presentation) { push(SyncCommand{reason}); return; }
        drain(3); std::lock_guard innerLock(innerMutex); inner->Sync(reason);
    }
    // Present: mostrar um quadro. No modo assincrono o jogo recebe o quadro
    // pronto mais recente (um VBlank atras) e no maximo 1 quadro fica
    // pendente; se a thread de desenho estiver mais lenta, o jogo espera
    // aqui (sem isso a fila crescia: segundos de atraso e gigabytes).
    PresentationFrame Present(const GSPresentationRequest &request) override {
        if (!asyncPresent) { drain(4); std::lock_guard innerLock(innerMutex); return inner->Present(request); }
        // The display conversion runs on the GS thread in queue order; the
        // caller gets the newest finished frame (one VBlank behind). At most
        // one frame stays in flight: when the GS thread is slower than the
        // game, an unbounded queue grows display latency and memory without
        // limit (menus with movies reached seconds of lag and gigabytes).
        push(PresentCommand{request});
        const uint64_t requested = ++presentsQueued;
        std::unique_lock lock(frameMutex);
        frameReady.wait(lock, [this, requested] {
            return framesPresented != 0 && framesPresented + 1 >= requested;
        });
        return lastFrame;
    }
    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override { drain(5); std::lock_guard innerLock(innerMutex); return inner->ClearFramebuffer(context, rgba); }
    uint32_t ConsumeLocalToHostBytes(uint8_t *out, uint32_t size) override { drain(6); std::lock_guard innerLock(innerMutex); return inner->ConsumeLocalToHostBytes(out, size); }
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override {
        drain(7);
        std::lock_guard innerLock(innerMutex);
        return inner->ReadVram(psm, base, bw, x, y);
    }
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override {
        push(WriteVramCommand{psm, base, bw, x, y, value});
    }
    void SnapshotVram(std::vector<uint8_t> &out) const override { drain(8); std::lock_guard innerLock(innerMutex); inner->SnapshotVram(out); }
    GSTransferSnapshot GetTransferSnapshot() const override { drain(9); std::lock_guard innerLock(innerMutex); return inner->GetTransferSnapshot(); }

private:
    // Constructs the command in place: one copy of a primitive, not three.
    template <typename... Args>
    // push: coloca um comando no fim da fila (construido no lugar, sem copias
    // extras) e acorda o worker se ele estiver dormindo.
    void push(Args &&...args) {
        bool notify;
        {
            std::lock_guard lock(mutex);
            if (queue.empty() || queue.back()->items.size() == kChunkSize) {
                if (spare.empty())
                    queue.push_back(std::make_unique<CommandChunk>());
                else {
                    queue.push_back(std::move(spare.back()));
                    spare.pop_back();
                }
            }
            queue.back()->items.emplace_back(std::forward<Args>(args)...);
            ++queued;
            notify = sleeping;
        }
        if (notify) wake.notify_one();
    }
    // drain: espera ate a thread executar TODOS os comandos enfileirados.
    // "reason" so serve para a estatistica do log (indice no vetor names).
    void drain(size_t reason) const {
        const auto start = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex);
        idle.wait(lock, [this] { return completed == queued; });
        ++drains[reason];
        waitMicroseconds[reason] += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
    }
    // execute: roda um comando no renderizador real (na thread worker).
    void execute(Command &command) {
        std::visit([this](auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, GSPrimitiveBatch>) inner->Submit(value);
            else if constexpr (std::is_same_v<T, LoadClutCommand>) inner->LoadClut(value.tex0, value.texclut);
            else if constexpr (std::is_same_v<T, GSTransferCommand>) inner->BeginTransfer(value);
            else if constexpr (std::is_same_v<T, UploadCommand>) inner->UploadImage(value.data.data(), uint32_t(value.data.size()));
            else if constexpr (std::is_same_v<T, FlushCommand>) inner->Flush();
            else if constexpr (std::is_same_v<T, TextureFlushCommand>) inner->TextureFlush();
            else if constexpr (std::is_same_v<T, SyncCommand>) inner->Sync(value.reason);
            else if constexpr (std::is_same_v<T, PresentCommand>) {
                PresentationFrame frame = inner->Present(value.request);
                {
                    std::lock_guard lock(frameMutex);
                    lastFrame = std::move(frame);
                    ++framesPresented;
                }
                frameReady.notify_all();
            }
            else inner->WriteVram(value.psm, value.base, value.bw, value.x, value.y, value.value);
        }, command);
    }
    // run: laco da thread worker. Dorme ate haver comandos, pega a fila
    // inteira, executa tudo com innerMutex travado (para nao misturar com uma
    // chamada direta de outra thread), devolve os blocos vazios para reuso
    // (spare) e avisa quem estiver em drain().
    void run() {
        std::vector<std::unique_ptr<CommandChunk>> batch;
        for (;;) {
            {
                std::unique_lock lock(mutex);
                sleeping = true;
                wake.wait(lock, [this] { return stopping || !queue.empty(); });
                sleeping = false;
                if (queue.empty() && stopping) return;
                batch.swap(queue);
            }
            uint64_t count = 0;
            {
                // Presentation can reach the wrapped backend from another
                // thread; never let it overlap a queued command.
                std::lock_guard innerLock(innerMutex);
                for (auto &chunk : batch) {
                    for (auto &command : chunk->items) execute(command);
                    count += chunk->items.size();
                    chunk->items.clear();
                }
            }
            {
                std::lock_guard lock(mutex);
                completed += count;
                for (auto &chunk : batch) spare.push_back(std::move(chunk));
            }
            batch.clear();
            idle.notify_all();
        }
    }

    std::unique_ptr<GSRasterBackend> inner;
    mutable std::mutex innerMutex;
    mutable std::mutex mutex;
    mutable std::condition_variable wake, idle;
    // Commands are large (a primitive carries its whole draw state). The queue
    // is a list of fixed-capacity chunks recycled through `spare`, so filling
    // it never moves commands (std::deque allocated one block per command in
    // MSVC; a growing std::vector moved thousands of them per frame).
    static constexpr size_t kChunkSize = 512;
    struct CommandChunk {
        std::vector<Command> items;
        CommandChunk() { items.reserve(kChunkSize); }
    };
    std::vector<std::unique_ptr<CommandChunk>> queue, spare;
    uint64_t queued{}, completed{};
    bool sleeping{}, stopping{};
    mutable std::array<uint64_t, 10> drains{}, waitMicroseconds{};
    bool asyncPresent = true;
    std::mutex frameMutex;
    std::condition_variable frameReady;
    PresentationFrame lastFrame;
    uint64_t framesPresented{};
    std::atomic<uint64_t> presentsQueued{};
    std::thread worker;
};
}

// createOrochiThreadedBackend: cria o embrulho com thread para "inner".
// Exemplo (main.cpp): createOrochiThreadedBackend(createOrochiGpuBackend())
std::unique_ptr<GSRasterBackend> createOrochiThreadedBackend(std::unique_ptr<GSRasterBackend> inner) {
    return std::make_unique<ThreadedBackend>(std::move(inner));
}

namespace {
// makeCpuBackend: cria um renderizador por CPU com N faixas paralelas
// (PS2X_GS_RASTER_THREADS), so para o teste abaixo.
std::unique_ptr<GSCpuBackend> makeCpuBackend(const char *threads) {
#ifdef _WIN32
    _putenv_s("PS2X_GS_RASTER_THREADS", threads);
#else
    setenv("PS2X_GS_RASTER_THREADS", threads, 1);
#endif
    auto backend = std::make_unique<GSCpuBackend>();
#ifdef _WIN32
    _putenv_s("PS2X_GS_RASTER_THREADS", "");
#else
    unsetenv("PS2X_GS_RASTER_THREADS");
#endif
    return backend;
}

// Serial and band-parallel rasterization of the same random primitives must
// leave identical GS memory: textured/untextured triangles and sprites,
// Gouraud, blending, depth tests, 32/16-bit targets, and textures that alias
// the target (which must take the serial path).
bool runBandRasterComparison() {
    constexpr uint32_t vramSize = 4u * 1024u * 1024u;
    std::vector<uint8_t> serialVram(vramSize), parallelVram(vramSize);
    std::mt19937 random(0x0c41u);
    for (auto &byte : serialVram) byte = uint8_t(random());
    parallelVram = serialVram;
    auto serial = makeCpuBackend("1");
    auto parallel = makeCpuBackend("4");
    serial->Initialize(serialVram.data(), vramSize);
    parallel->Initialize(parallelVram.data(), vramSize);
    auto pick = [&](uint32_t bound) { return uint32_t(random() % bound); };
    bool readsMatch = true;
    for (int i = 0; i < 3000; ++i) {
        GSPrimitiveBatch batch{};
        auto &state = batch.state;
        auto &ctx = state.context;
        const bool sprite = pick(4) == 0;
        state.prim.type = sprite ? GS_PRIM_SPRITE : GS_PRIM_TRIANGLE;
        batch.vertexCount = sprite ? 2 : 3;
        state.prim.iip = pick(2);
        state.prim.tme = pick(3) != 0;
        state.prim.abe = pick(2);
        state.prim.fst = pick(2);
        ctx.frame = {0u, 10u, uint8_t(pick(4) == 0 ? GS_PSM_CT16 : GS_PSM_CT32), 0u};
        ctx.zbuf = {150u, uint8_t(pick(3) == 0 ? GS_PSM_Z16 : GS_PSM_Z32), pick(4) == 0};
        ctx.scissor = {0u, 639u, 0u, 447u};
        ctx.test = (1ull << 16) | (uint64_t(1 + pick(3)) << 17);
        ctx.alpha = uint64_t(pick(256)) | (uint64_t(pick(256)) << 32);
        const bool aliasTarget = pick(20) == 0;
        ctx.tex0.tbp0 = aliasTarget ? 0u : 9600u;
        ctx.tex0.tbw = 4u;
        ctx.tex0.psm = GS_PSM_CT32;
        ctx.tex0.tw = ctx.tex0.th = 8u;
        ctx.tex0.tcc = uint8_t(pick(2));
        ctx.tex0.tfx = uint8_t(pick(4));
        state.textureWidth = state.textureHeight = 256u;
        for (unsigned v = 0; v < batch.vertexCount; ++v) {
            auto &vertex = batch.vertices[v];
            vertex.x = float(pick(640));
            vertex.y = float(pick(448));
            vertex.z = double(pick(0x10000));
            vertex.r = uint8_t(pick(256)); vertex.g = uint8_t(pick(256));
            vertex.b = uint8_t(pick(256)); vertex.a = uint8_t(pick(256));
            vertex.u = uint16_t(pick(256 * 16)); vertex.v = uint16_t(pick(256 * 16));
            vertex.s = float(pick(1000)) / 1000.0f; vertex.t = float(pick(1000)) / 1000.0f;
            vertex.q = 0.5f + float(pick(1000)) / 1000.0f;
            vertex.fog = uint8_t(pick(256));
        }
        serial->Submit(batch);
        parallel->Submit(batch);
        if (i % 401 == 0) {
            const uint32_t x = pick(640), y = pick(448);
            readsMatch &= serial->ReadVram(GS_PSM_CT32, 0u, 10u, x, y) == parallel->ReadVram(GS_PSM_CT32, 0u, 10u, x, y);
        }
    }
    serial->Sync(GSSyncReason::Finish);
    parallel->Sync(GSSyncReason::Finish);
    const bool same = readsMatch && serialVram == parallelVram;
    std::cout << "[GS thread regression] 4-band parallel raster matches serial raster (3000 primitives): "
              << (same ? "PASS" : "FAIL") << '\n';
    if (!same) {
        size_t first = vramSize, count = 0;
        for (size_t i = 0; i < vramSize; ++i)
            if (serialVram[i] != parallelVram[i]) { if (first == vramSize) first = i; ++count; }
        std::cout << "  readsMatch=" << readsMatch << " firstDiff=0x" << std::hex << first << std::dec
                  << " page=" << first / 8192 << " differingBytes=" << count << '\n';
    }
    return same;
}
}

// runGsThreadRegressionTests (--test-gs-thread): o mesmo fluxo de comandos
// direto e pela thread precisa deixar a VRAM identica byte a byte; e o
// desenho em 4 faixas paralelas precisa bater com o desenho serial.
int runGsThreadRegressionTests() {
    // Same command stream through the CPU backend directly and through the
    // worker thread: VRAM must be byte-identical, including reads in between.
    constexpr uint32_t vramSize = 4u * 1024u * 1024u;
    std::vector<uint8_t> direct(vramSize), threaded(vramSize);
    GSCpuBackend reference;
    reference.Initialize(direct.data(), vramSize);
    auto wrapped = createOrochiThreadedBackend(std::make_unique<GSCpuBackend>());
    wrapped->Initialize(threaded.data(), vramSize);
    bool readsMatch = true;
    for (uint32_t i = 0; i < 20000; ++i) {
        const uint32_t x = (i * 37u) % 640u, y = (i * 11u) % 448u, value = i * 2654435761u;
        reference.WriteVram(0, 0, 10, x, y, value);
        wrapped->WriteVram(0, 0, 10, x, y, value);
        if (i % 997 == 0)
            readsMatch &= reference.ReadVram(0, 0, 10, x, y) == wrapped->ReadVram(0, 0, 10, x, y);
    }
    GSPrimitiveBatch sprite{};
    sprite.vertexCount = 2;
    sprite.state.prim.type = GS_PRIM_SPRITE;
    sprite.state.context.frame.fbw = 10;
    sprite.state.context.scissor.x1 = 639;
    sprite.state.context.scissor.y1 = 447;
    sprite.state.context.zbuf.zmask = 1;
    sprite.vertices[0].x = 10; sprite.vertices[0].y = 20;
    sprite.vertices[1].x = 300; sprite.vertices[1].y = 200;
    sprite.vertices[1].r = 0x40; sprite.vertices[1].g = 0x80; sprite.vertices[1].b = 0xC0; sprite.vertices[1].a = 0x80;
    for (int repeat = 0; repeat < 50; ++repeat) {
        sprite.vertices[1].r = uint8_t(repeat * 5);
        reference.Submit(sprite);
        wrapped->Submit(sprite);
    }
    reference.Sync(GSSyncReason::Finish);
    wrapped->Sync(GSSyncReason::Finish);
    const bool same = readsMatch && std::memcmp(direct.data(), threaded.data(), vramSize) == 0;
    std::cout << "[GS thread regression] queued writes/draws match direct backend: " << (same ? "PASS" : "FAIL") << '\n';
    const bool bands = runBandRasterComparison();
    return same && bands ? 0 : 1;
}
