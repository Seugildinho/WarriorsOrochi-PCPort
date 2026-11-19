// ==========================================================================
// DISTANCIA DE DESENHO DOS SOLDADOS ("Soldados desenhados" no Setup)
// ==========================================================================
// No jogo original, so os N personagens mais proximos sao desenhados de
// corpo inteiro; os outros aparecem so como barra de vida flutuando. Isso
// e o "pop-in" de soldados. Este arquivo aumenta esse N.
// Valor configurado: OROCHI_CROWD (Setup, aba Graficos). 51 = original.
#include "draw_distance.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

// Read every time the game sets the limit; tests change it in a running game.
// g_orochiCrowdTarget: quantos personagens o usuario quer (ex.: 100).
// Global e lida a cada chamada para que testes possam muda-la com o jogo
// rodando.
int32_t g_orochiCrowdTarget=0;

namespace {
// How many characters the game draws in full. Each frame it ranks the
// officers and soldiers on the field by distance (orochi_block_197648) and
// only the nearest N get a body; the rest show just their health bar until
// they come closer. FUN_001f2110 sets N in gp-0x7B2C: a per-stage value
// (at most 51), 30 with two players, 20 in one special mode. The ranking
// loop only compares against it, so a larger N needs no bigger tables; the
// pool holds 147 characters.
// Enderecos do jogo (versao USA, SLUS-21662):
//   limitPc      : funcao do jogo que define o limite N da fase (0x1f2110).
//   limitAddress : onde o limite fica na memoria (gp - 0x7B2C).
//   gameMaximum  : maior limite que o jogo usa (51).
//   poolSize     : quantos personagens a tabela comporta (147); nunca
//                  passamos disso.
constexpr uint32_t limitPc=0x1f2110u;
constexpr uint32_t gp=0x360170u,limitAddress=gp-0x7B2Cu;
constexpr int32_t gameMaximum=51,poolSize=147;

// limitOriginal: a funcao original do jogo, chamada antes do nosso ajuste.
PS2Runtime::RecompiledFunction limitOriginal{};

// setLimit: substitui a funcao 0x1f2110 do jogo.
//   1. Roda a original (ela grava o limite da fase).
//   2. Le esse limite e aumenta NA MESMA PROPORCAO:
//        novo = limite * alvo / 51, no maximo 147.
//   3. So grava se o novo valor for maior (nunca reduz).
// Exemplos com alvo 100: fase com 51 -> 100; dois jogadores (30) -> 58;
// fase com 40 -> 78. Com alvo 147: 51 -> 147.
void setLimit(uint8_t *ram,R5900Context *ctx,PS2Runtime *runtime) {
    limitOriginal(ram,ctx,runtime);
    int32_t limit;
    std::memcpy(&limit,ram+limitAddress,4);
    // Scale the stage's own value, so busier stages stay busier.
    const int32_t raised=std::min(poolSize,limit*g_orochiCrowdTarget/gameMaximum);
    if(raised>limit) std::memcpy(ram+limitAddress,&raised,4);
}
}

// installOrochiDrawDistance: chamada por main.cpp. Sem OROCHI_CROWD, ou com
// valor ate 51, nao muda nada (comportamento original) e retorna true.
// Acima de 51, troca a funcao 0x1f2110 por setLimit. Retorna false so se a
// funcao original nao existir no codigo gerado.
bool installOrochiDrawDistance(PS2Runtime &runtime) {
    const char *value=std::getenv("OROCHI_CROWD");
    if(!value) return true;
    g_orochiCrowdTarget=std::clamp(std::atoi(value),0,poolSize);
    if(g_orochiCrowdTarget<=gameMaximum) return true;
    std::cout << "[Orochi:draw] characters drawn in full: up to " << g_orochiCrowdTarget << " (game: " << gameMaximum << ")\n";
    limitOriginal=runtime.lookupFunction(limitPc);
    return limitOriginal && runtime.replaceFunction(limitPc,setLimit);
}
