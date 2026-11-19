// ==========================================================================
// OVERLAYS: PEDACOS DO JOGO CARREGADOS DO DISCO DURANTE A PARTIDA
// ==========================================================================
// O Warriors Orochi nao cabe todo na memoria: partes do codigo (overlays)
// sao lidas do disco quando precisa, SEMPRE no mesmo endereco 0x819000:
//   ovl_00.bin -> modo de jogo principal (batalhas, historia)
//   ovl_01.bin -> menus OPTIONS e GALLERY
// Problema para um port recompilado: as duas versoes ocupam o MESMO
// endereco, mas o codigo C++ gerado de cada uma e diferente. Solucao:
//   1. Interceptamos o carregador do jogo (0x151f40).
//   2. Depois que ele carrega um overlay, conferimos que os bytes sao
//      exatamente os que foram recompilados (impressao digital FNV-1a).
//   3. Trocamos a tabela de funcoes do intervalo 0x819000-0x83d800 para as
//      funcoes compiladas daquele overlay (activateOverlayEntries).
// Se o disco tiver um overlay diferente do compilado (outra versao do jogo)
// o jogo PARA com "[Orochi:blocker]" no log, em vez de rodar codigo errado.
#include "native_overlay.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <orochi_supplemental_entries.h>
#include <iostream>
#include <memory>
#include <vector>
#include <cstring>

namespace {
// orochiOverlayLoader: a funcao original do jogo que carrega overlays.
PS2Runtime::RecompiledFunction orochiOverlayLoader{};
// Both disc modules load at the same address, so only one has live entries in
// the function table; loadedOverlay is the module those entries belong to.
constexpr int overlayNone=-1;
int loadedOverlay{overlayNone};
constexpr uint32_t overlayBase=0x819000;
// Every address either module (code, data, bss) can occupy, from the ELF's PT_LOADs.
constexpr uint32_t overlayEnd=0x83d800;
// OverlayModule: nome do arquivo no disco, tamanho em bytes e a impressao
// digital esperada (hash FNV-1a 64 bits dos bytes carregados).
struct OverlayModule {const char *name;uint32_t bytes;uint64_t fingerprint;};
// Fingerprints of the exact ISO bytes documented in config/overlay_0N.json.
constexpr OverlayModule overlayModules[]={
    {"ovl_00.bin",142400,0x2c2a87266123e930ull},
    {"ovl_01.bin",48256,0x29be16fadd7352b9ull},
};
constexpr uint32_t overlayModuleCount=sizeof(overlayModules)/sizeof(overlayModules[0]);
// Overlay 0 is part of the main generation: its entries are what the table holds
// at startup, kept here so it can be restored after another module replaced it.
std::vector<OrochiSupplementalEntry> overlay00Entries;
constexpr uint32_t attractReturnLeaf=0x1f2360;

// returnFromOriginalLeaf: implementacao de uma funcao vazia do jogo
// (0x1f2360, "JR $ra; NOP"): apenas retorna, sem mudar nenhum registrador.
// bindOriginalReturnLeaf so a registra se a memoria tiver exatamente essas
// duas instrucoes.
void returnFromOriginalLeaf(uint8_t *,R5900Context *ctx,PS2Runtime *) {
    // The original function is exactly JR $ra; NOP. Preserve every register,
    // including v0: this is an actual empty leaf, not a fabricated success code.
    ctx->pc=getRegU32(ctx,31);
}
bool bindOriginalReturnLeaf(PS2Runtime &runtime) {
    if(runtime.memory().read32(attractReturnLeaf)!=0x03e00008u ||
       runtime.memory().read32(attractReturnLeaf+4)!=0u) return false;
    if(runtime.hasFunction(attractReturnLeaf)) return true;
    return runtime.registerFunction(attractReturnLeaf,returnFromOriginalLeaf);
}

// activateOverlayEntries: deixa ativas na tabela so as funcoes do overlay
// "module" (0 ou 1).
//   1. Remove TODAS as funcoes registradas entre 0x819000 e 0x83d800.
//   2. Registra as do modulo pedido (ovl_00: lista guardada no inicio;
//      ovl_01: lista gerada orochiOverlay01Entries).
// Exemplo: entrar em OPTIONS -> module=1 -> 0x8194d0 passa a existir e
// 0x81cf50 (so do ovl_00) deixa de existir. Voltar -> module=0 desfaz.
bool activateOverlayEntries(PS2Runtime &runtime,uint32_t module) {
    if(module>=overlayModuleCount) return false;
    for(uint32_t address=overlayBase;address<overlayEnd;address+=4)
        if(runtime.hasFunction(address) && !runtime.replaceFunction(address,nullptr)) return false;
    if(module==0) {
        for(const auto &entry:overlay00Entries)
            if(!runtime.replaceFunction(entry.address,entry.function)) return false;
    } else {
        for(const auto &entry:orochiOverlay01Entries)
            if(!runtime.replaceFunction(entry.address,entry.function)) return false;
    }
    return true;
}

// loadOrochiOverlay: substitui o carregador do jogo (0x151f40).
//   - Na entrada: se o jogo pede um overlay que nao foi compilado
//     (numero >= 2), para o jogo ANTES de sobrescrever a memoria.
//   - Chama o carregador original (ele pode levar varios quadros; esta
//     funcao e chamada de novo a cada retomada).
//   - Quando o carregamento termina (flag gp-0x7c44 == 1), confere o hash
//     dos bytes, ativa as funcoes do modulo e marca a regiao como codigo.
// Enderecos usados: gp-0x7f18 = numero do overlay atual,
// gp-0x7c44 = 1 quando o carregamento terminou.
void loadOrochiOverlay(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    const auto gp=getRegU32(ctx,28);
    if(ctx->pc==0x151f40u) {
        const auto requested=getRegU32(ctx,4);
        // Modules share an address range. Reject unsupported code before the
        // original loader can overwrite it or invoke its constructors.
        if(requested>=overlayModuleCount) {
            std::cerr << "[Orochi:blocker] Overlay not compiled; requested=" << requested
                << " caller=0x" << std::hex << getRegU32(ctx,31) << std::dec << '\n';
            runtime->requestStop(); return;
        }
        if(requested!=runtime->memory().read8(gp-0x7f18u)) loadedOverlay=overlayNone;
    }
    orochiOverlayLoader(ram,ctx,runtime);
    // The original loader sets this flag only at its completed return path.
    if(loadedOverlay!=overlayNone || ctx->pc!=getRegU32(ctx,31) || runtime->isStopRequested() ||
       runtime->memory().read32(gp-0x7c44u)!=1u) return;
    const uint32_t mode=runtime->memory().read8(gp-0x7f18u);
    uint64_t hash=0xcbf29ce484222325ull;
    if(mode<overlayModuleCount)
        for(uint32_t i=0;i<overlayModules[mode].bytes;i++) hash=(hash^ram[overlayBase+i])*0x100000001b3ull;
    if(mode>=overlayModuleCount || hash!=overlayModules[mode].fingerprint || !activateOverlayEntries(*runtime,mode)) {
        std::cerr << "[Orochi:blocker] Loaded overlay differs from compiled code; mode="
            << mode << " fingerprint=0x" << std::hex << hash << std::dec << '\n';
        runtime->requestStop(); return;
    }
    loadedOverlay=int(mode);
    runtime->memory().registerCodeRegion(overlayBase+0x80,overlayBase+overlayModules[mode].bytes);
    std::cout << "[Orochi:overlay] " << overlayModules[mode].name << " verified against compiled code; bytes="
        << overlayModules[mode].bytes << '\n';
}
}

// installOrochiSupplementalFunctions: registra funcoes "suplementares"
// (trechos que a analise automatica nao achou e que foram gerados a parte,
// tools/generate_supplemental.py). Antes confere o hash de cada intervalo
// na memoria; se o ELF for diferente do esperado, falha (main.cpp sai).
bool installOrochiSupplementalFunctions(PS2Runtime &runtime) {
    for(const auto &range:orochiSupplementalRanges) {
        uint64_t hash=0xcbf29ce484222325ull;
        for(auto address=range.start;address<range.end;++address)
            hash=(hash^runtime.memory().read8(address))*0x100000001b3ull;
        if(hash!=range.checksum) return false;
    }
    for(const auto &entry:orochiSupplementalEntries)
        if(!runtime.hasFunction(entry.address) && !runtime.registerFunction(entry.address,entry.function)) return false;
    return true;
}

// installOrochiOverlayFunctions: chamada uma vez por main.cpp.
//   1. Registra a funcao vazia 0x1f2360.
//   2. Guarda as funcoes do ovl_00 que estao na tabela no inicio (para
//      restaurar depois que o ovl_01 for usado).
//   3. Troca o carregador 0x151f40 (e seus pontos de retomada ate
//      0x15201c) por loadOrochiOverlay.
bool installOrochiOverlayFunctions(PS2Runtime &runtime) {
    if(!bindOriginalReturnLeaf(runtime)) return false;
    loadedOverlay=overlayNone;
    overlay00Entries.clear();
    for(uint32_t address=overlayBase;address<overlayEnd;address+=4)
        if(runtime.hasFunction(address)) overlay00Entries.push_back({address,runtime.lookupFunction(address)});
    orochiOverlayLoader=runtime.lookupFunction(0x151f40);
    if(!orochiOverlayLoader || orochiOverlayLoader==loadOrochiOverlay) return false;
    for(uint32_t pc=0x151f40;pc<0x15201c;pc+=4) {
        if(runtime.hasFunction(pc) && runtime.lookupFunction(pc)==orochiOverlayLoader &&
            !runtime.replaceFunction(pc,loadOrochiOverlay)) return false;
    }
    return true;
}

// runOverlayRegressionTests (WarriorsOrochi.exe --test-overlay): testa sem
// ISO a funcao vazia, duas funcoes de geometria, a troca de tabela entre
// ovl_00/ovl_01 nos dois sentidos e a recusa de um overlay nao compilado.
int runOverlayRegressionTests() {
    auto runtime=std::make_unique<PS2Runtime>();
    if(!runtime->memory().initialize()) return 2;
    const auto previous=runtime->lookupFunction(attractReturnLeaf);
    const bool rejectsMismatch=!bindOriginalReturnLeaf(*runtime) && runtime->lookupFunction(attractReturnLeaf)==previous;
    runtime->memory().write32(attractReturnLeaf,0x03e00008u);
    runtime->memory().write32(attractReturnLeaf+4,0);
    if(!installOrochiOverlayFunctions(*runtime)) return 2;
    R5900Context leaf{};leaf.pc=attractReturnLeaf;
    for(unsigned i=1;i<32;++i) leaf.r[i]=_mm_set_epi64x(0x123456789abcdef0ull,0x55000000u+i);
    leaf.r[31]=_mm_set_epi64x(0,0x100638);
    const auto originalLeaf=leaf;
    runtime->lookupFunction(attractReturnLeaf)(runtime->memory().getRDRAM(),&leaf,runtime.get());
    const bool leafMatches=rejectsMismatch && leaf.pc==0x100638 &&
        !std::memcmp(leaf.r,originalLeaf.r,sizeof(leaf.r)) &&
        runtime->memory().read32(attractReturnLeaf)==0x03e00008u && runtime->memory().read32(attractReturnLeaf+4)==0;
    std::cout << "[Overlay regression] traced idle leaf requires exact JR/NOP and preserves all registers: "
        << (leafMatches ? "PASS" : "FAIL") << '\n';
    std::memset(runtime->memory().getScratchpad(),0,PS2_SCRATCHPAD_SIZE);
    // The original first guard adds these two scratchpad floats.
    runtime->memory().write32(0x700006e0,0x3f800000); // +1
    runtime->memory().write32(0x700006e8,0xc0000000); // -2
    R5900Context geometry{};geometry.pc=0x294ba0;geometry.r[31]=_mm_set_epi64x(0,0x221560);
    geometry.r[2]=_mm_set_epi64x(0,0xdeadbeef);
    orochi_traced_294ba0_0x294ba0(runtime->memory().getRDRAM(),&geometry,runtime.get());
    const bool geometryMatches=geometry.pc==0x221560 && getRegU32(&geometry,2)==0 &&
        geometry.f[8]==1.f && geometry.f[9]==-2.f && geometry.f[7]==-1.f;
    std::cout << "[Overlay regression] traced geometry reads scratchpad and rejects a non-positive input sum: "
        << (geometryMatches ? "PASS" : "FAIL") << '\n';
    R5900Context otherGeometry{};otherGeometry.pc=0x2949a0;
    otherGeometry.r[31]=_mm_set_epi64x(0,0x221560);
    orochi_traced_2949a0_0x2949a0(runtime->memory().getRDRAM(),&otherGeometry,runtime.get());
    const bool otherGeometryMatches=otherGeometry.pc==0x221560 && getRegU32(&otherGeometry,2)==0 &&
        otherGeometry.f[8]==-2.f && otherGeometry.f[9]==1.f && otherGeometry.f[7]==-3.f;
    std::cout << "[Overlay regression] second traced geometry rejects a non-positive difference: "
        << (otherGeometryMatches ? "PASS" : "FAIL") << '\n';
    // Same address, different module: only the loaded one may answer. 0x8194d0 is
    // ovl_01's Options/Gallery entry; 0x81cf50 exists only in ovl_00.
    const auto ovl00At819510=runtime->lookupFunction(0x819510);
    const bool swaps=activateOverlayEntries(*runtime,1) && runtime->hasFunction(0x8194d0) && !runtime->hasFunction(0x81cf50) &&
        runtime->lookupFunction(0x819510)!=ovl00At819510;
    const bool restores=activateOverlayEntries(*runtime,0) && !runtime->hasFunction(0x8194d0) && runtime->hasFunction(0x81cf50) &&
        runtime->lookupFunction(0x819510)==ovl00At819510;
    std::cout << "[Overlay regression] modules sharing an address swap the function table both ways: "
        << (swaps && restores ? "PASS" : "FAIL") << '\n';
    auto *ram=runtime->memory().getRDRAM();
    const uint32_t overlay00Size=overlayModules[0].bytes;
    std::memset(ram+overlayBase,0xa5,overlay00Size);
    const std::vector<uint8_t> before(ram+overlayBase,ram+overlayBase+overlay00Size);
    R5900Context ctx{};
    auto reg=[&](unsigned index,uint32_t value) {ctx.r[index]=_mm_set_epi64x(0,value);};
    reg(4,overlayModuleCount); reg(28,0x360170); reg(29,0x9000); reg(31,0x152030);
    runtime->memory().write8(0x360170-0x7f18,0);
    runtime->memory().write32(0x360170-0x7c44,1);
    ctx.pc=0x151f40;
    runtime->lookupFunction(ctx.pc)(ram,&ctx,runtime.get());
    const bool stopped=runtime->isStopRequested();
    const bool unchanged=ctx.pc==0x151f40 && getRegU32(&ctx,29)==0x9000 &&
        getRegU32(&ctx,31)==0x152030 && runtime->memory().read8(0x360170-0x7f18)==0 &&
        runtime->memory().read32(0x360170-0x7c44)==1 &&
        !std::memcmp(before.data(),ram+overlayBase,overlay00Size);
    std::cout << "[Overlay regression] unsupported module stops before loader writes or constructors: "
        << (stopped && unchanged?"PASS":"FAIL") << '\n';
    return stopped && unchanged && leafMatches && geometryMatches && otherGeometryMatches && swaps && restores?0:1;
}
