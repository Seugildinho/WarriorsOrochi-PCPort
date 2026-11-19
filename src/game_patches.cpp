// Values of the runtime-selectable game patches (src/game_patches.h).
// Como os patches de jogo funcionam:
//   config/game_patches.json lista instrucoes do jogo original a trocar.
//   tools/apply_game_patches.py reescreve essas instrucoes no codigo
//   gerado (generated/) para algo como:
//     if (OrochiGamePatches::widescreen) { <versao 16:9> } else { <original> }
//   Este arquivo so define os valores dessas chaves ao abrir o jogo.
#include "game_patches.h"

#include <cstdlib>

namespace
{
    // environmentFlag: true se a variavel existir e nao for vazia nem "0".
    // Exemplos: OROCHI_WIDESCREEN=1 -> true; =0 -> false; ausente -> false.
    bool environmentFlag(const char *name)
    {
        const char *value = std::getenv(name);
        return value && *value && *value != '0';
    }
}

namespace OrochiGamePatches
{
    // widescreen: comeca pelo ambiente; applyOrochiSettings troca pelo valor
    // do Setup (aspect=16:9) se OROCHI_WIDESCREEN nao foi definida.
    // fixedFieldOffset: ligado por padrao (tira o tremor de meia linha);
    // OROCHI_FIELD_OFFSET=1 volta ao comportamento original do PS2.
    bool widescreen = environmentFlag("OROCHI_WIDESCREEN");
    bool fixedFieldOffset = !environmentFlag("OROCHI_FIELD_OFFSET");
}
