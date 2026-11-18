# Warriors Orochi — protótipo nativo a partir do PS2

Projeto experimental para executar no Windows o código da versão de PS2
de **Warriors Orochi (USA), SLUS-21662, revisão 1.00**.

**Ainda não é um port jogável.** O primeiro objetivo é observar a inicialização
do código recompilado, identificar o primeiro erro real e corrigi-lo com evidências.
Compilar um executável não comprova funcionamento dos menus ou das batalhas.

As quinze suítes de regressão passam. A abertura exibe os logotipos da KOEI e
Omega Force e chega ao título. No teste de 291,20 segundos, START abriu o menu
principal; História abriu a seleção de histórias e o acampamento de Shu.
Também foram abertos os menus de armas e habilidades. A arma de Zhao Yun
mostrou nome, imagem e ataque; a lista inicial de habilidades estava vazia.

A integração agora confirma os pacotes realmente recebidos pelo decodificador
nativo e encaminha o fim de entrada e as consultas de estado para esse mesmo
decodificador. Não duplica os bytes na fila do IPU físico nem apaga contadores
para simular consumo. A execução termina normalmente pelo comando de teste.

Os registros ainda mostram falha na reserva do buffer de música após as
animações. Som audível e batalhas não foram validados. Missão de Ueda,
dificuldade e apresentação do capítulo foram vistas em builds anteriores e
precisam ser repetidas nesta compilação. Veja `PROGRESSO.md` e `RETOMADA.md`.

Repositório de backup **privado**, por solicitação do responsável. Não tornar
público sem uma nova decisão dele. Não há versão jogável para baixar.
O envio deste checkpoint ao GitHub privado foi autorizado em 22/10/2026.
Novas atualizações remotas dependem de outro pedido do responsável.

## Como jogar

O port tem dois programas, lado a lado em `build-tools/ps2xRuntime/Release`:

- `WarriorsOrochi-Setup.exe` (`Configurar port.cmd`): escolhe a ISO, em qualquer pasta, e as opções de vídeo, áudio e save automático.
- `WarriorsOrochi.exe` (`Abrir port.cmd`): abre o jogo com o que foi salvo no Setup. Sem ISO configurada, abre o Setup.

A ISO precisa ser do Warriors Orochi (USA) ou uma versão que mude apenas textos, como a tradução PT-BR. O executável do jogo é lido da própria ISO.

## Abordagem

Usa [PS2Recomp](https://github.com/ran-j/PS2Recomp), fixado no commit
`eb8688b2fd191129f103a39543002c005b33409c`, para converter o código do
Emotion Engine em C++ e compilá-lo para x64. A infraestrutura ainda fornece
serviços do PS2 e intérpretes para componentes auxiliares, como IOP e VU.
O objetivo é substituir progressivamente dependências específicas do console.
O iniciador normal usa renderização por CPU (`GSCpuBackend`). Existe um caminho
experimental Direct3D 11 Compute (`OROCHI_RENDERER=d3d11`) que acelera somente
sprites de cor constante; texturas e triângulos continuam na CPU. A janela usa
OpenGL/raylib. Não há renderização Vulkan nesta compilação.

O [port de ICO](https://github.com/nathanialf/ico-pc) foi uma referência de
arquitetura; seu código não foi incorporado a este protótipo.

## Base exata

- ISO: `Warriors Orochi (USA).iso`, 2.677.604.352 bytes.
- SHA-256 da ISO: `1c7a1ee34b56612593625ad327f38e3b955c850320b13cfaecada326f86df85e`.
- Executável original: `SLUS_216.62`, 2.462.208 bytes.
- SHA-256 do executável: `40e68b87a4a000000a1b22b07f71f601073109c77fa3b8840d5286afe2eb75db`.
- Entrada: `0x00100008`. Registrador GP inicial: `0x00360170`.
- Não há símbolos de funções: a seção de símbolos está vazia.

Esta etapa usa a ISO original para distinguir problemas do port de problemas
da tradução. A tradução PT-BR 1.0 e sua publicação permanecem em seu projeto.

## Ferramentas locais

Visual Studio 2022 Build Tools, CMake e Python 3.12. O adaptador IPU2 usa
libavcodec no próprio processo; FFmpeg 8.1 no PATH serve somente como referência
dos testes e para a comparação opcional com o caminho antigo. Para IPU2 e
MPEG-2, a compilação usa as bibliotecas compartilhadas do
FFmpeg 7.1, baixadas pelo CMake e verificadas por SHA-256. Os dados do pacote
estão em `config/ffmpeg-windows.json`; suas DLLs são copiadas ao lado do executável.
Ghidra 12.1.4, extensão
Emotion Engine Reloaded 2.1.38 e JDK 21 foram colocados em `vendor/`, sem mudar
o Java global. As versões e os hashes ficam em `config/` e em
`local/analysis_tools_paths.json`.

Use Python 3.12 ou superior, Git e Visual Studio 2022 Build Tools com C++ para
Windows. Clone este repositório numa pasta ao lado da ISO original, com o nome
exato `Warriors Orochi (USA).iso`. A pasta do repositório pode ter qualquer nome.
Prepare a dependência fixada a partir da raiz deste repositório:

```powershell
git clone https://github.com/ran-j/PS2Recomp.git vendor/PS2Recomp
git -C vendor/PS2Recomp checkout eb8688b2fd191129f103a39543002c005b33409c
```

Atalho: com GNU make instalado (`winget install ezwinports.make`), `make help` lista os passos. Na primeira vez: `make deps` (instala o que faltar e baixa PS2Recomp e PCSX2), `make check`, `make generate` e `make install`; depois de editar `src/`, só `make`. `make reinstall`, depois de alterar o código, recompila só o que mudou (inclusive `.cpp` novos e patches) e troca os programas da pasta do jogo, mantendo saves e configurações, `make test` roda as regressões e `make package` gera um zip com o programa, o código e os logs.

Sequência completa, a partir desta pasta, com Python e FFmpeg disponíveis:

```powershell
python tools/inspect_game.py
python tools/workflow.py tools
python tools/workflow.py analyze
python tools/download_analysis_tools.py
python tools/analyze_ghidra.py
python tools/workflow.py generate
python tools/workflow.py native
python tools/run_regressions.py
python tools/run_probe.py --seconds 15
```

O checkout fixado de PS2Recomp precisa existir em `vendor/PS2Recomp`.
O gerador arquiva automaticamente a rodada anterior em `local/generated-DATA-HORA`
antes de gerar uma nova pasta, para não misturar funções antigas e novas.
As correções locais em `patches/` são aplicadas pela configuração da compilação.
O mapa usado é `local/analysis/orochi.ranges.csv`, com intervalos de instruções
sem sobreposição. O checkpoint inclui `config/orochi.ranges.csv`, somente com nomes
e endereços, para conservar o mapa testado. Na ausência do mapa local, o gerador
copia esse checkpoint. Para reproduzir o código atual, é possível pular o download
e a execução do Ghidra: execute `inspect_game`, `workflow tools`, `workflow analyze`,
`workflow generate` e `workflow native` nessa ordem.
O gerador rejeita mapas ausentes ou sobrepostos. Quinze rotinas
de configuração e envio gráfico usam o código original recompilado, conforme
`config/analysis.json`, para preservar a estrutura desta versão do SDK.

Para atualizar a análise com novas evidências sem reimportar o executável, use
`python tools/analyze_ghidra.py --update`. As funções comprovadas por execução ficam
em `config/analysis.json`; a recuperação adicional por referências gera
`local/analysis/indirect_candidates.csv`.

O jogo também carrega código do disco que não está no executável inicial.
O primeiro módulo identificado é `ovl_00.bin`, com 142.400 bytes. A configuração
conserva sua posição na ISO e o SHA-256 em `config/overlay_00.json`.
`workflow generate` extrai esse módulo da ISO e prepara um ELF adicional **somente
para gerar C++**; o protótipo continua carregando o ELF original e deixa o próprio
jogo ler o módulo do disco. A integração verifica os bytes carregados antes de
liberar seu código recompilado.

`ovl_01.bin` (OPTIONS e GALLERY, 48.256 bytes, `config/overlay_01.json`) usa o mesmo
endereço, então não cabe na geração principal: `tools/generate_supplemental.py` o
recompila à parte com símbolos `ovl01_*` e a carga troca as entradas da tabela de
funções depois de verificar os bytes. O disco só tem esses dois módulos.
Para repetir a análise com Ghidra, execute `python tools/analyze_overlay.py --index N`.
Os mapas testados estão em `config/overlay_00.ranges.csv` e `config/overlay_01.ranges.csv`.

## Diagnósticos e limites

- Inventário verificado: `relatorios/inventario_ps2.json`.
- Análise do executável: `logs/analyze.log` e `logs/ghidra.log`.
- Conversão: `logs/generate.log`.
- Compilação: `logs/build-native.log`.
- Teste de inicialização: `relatorios/teste_boot.json` e `logs/boot.*.log`.
- `--stack` no teste registra a pilha nativa da thread do próprio protótipo.
- `relatorios/boot-frame.png` registra a imagem desenhada pelo protótipo.
- Execute o binário com `--test-dma` para os testes de endereçamento e estado DMA.
- `--test-vsync` verifica o argumento, GP e pilha de três callbacks consecutivos.
- `--test-gif` compara um envio de imagem completo com o mesmo envio fragmentado.
- `--test-display` verifica os modos de exibição progressivo e entrelaçado.
- `--test-pad` verifica tamanho do pacote, aceitação pelo chamador original,
  falha de porta fechada e proteção dos limites da memória.
- `--test-sif` verifica a entrega de um pacote do transporte do IOP ao handler
  original do SDK e ao leitor de estado de som usado pelo jogo. Reinicializar
  os handlers conserva o estado já recebido.
- `--test-iop-memory` verifica 100 ciclos de alocar e liberar buffers de música,
  reutilização de espaços livres, limites, alinhamento e consultas de memória.
- `--test-iop-loader` verifica a reserva conjunta de módulos e buffers, a
  liberação de imagens e a rejeição de imagens inválidas ou sobrepostas.
- `--test-syncv` executa a espera original da animação com os dois campos de
  vídeo entrelaçado e verifica o retorno do modo progressivo.
- `--test-mpeg-audio` verifica os seletores originais 3 e 4, pacotes divididos,
  rejeição de outro canal e a cópia do cabeçalho pelo callback do próprio jogo.
- `--test-mpeg-transport` verifica a confirmação dos bytes já recebidos,
  rejeita confirmações incorretas ou repetidas e conserva os quadros pendentes
  até o fim da apresentação. A fila física do IPU permanece sem alterações.
- `--test-overlay` verifica que um módulo não recompilado é recusado antes
  de substituir o código na memória ou executar construtores, e que os módulos
  de mesmo endereço trocam a tabela de funções nos dois sentidos.
- `--test-switch` executa as oito entradas da tabela de Ueda, em ambos os
  caminhos de condição, verificando os resultados do próprio código recompilado.
- `python tools/run_probe.py --seconds 30 --press-start` injeta START somente no
  controle interno deste protótipo, para diagnosticar a transição da tela de título.
- Exemplo de sequência programada para História; os tempos podem precisar de
  ajuste porque as animações iniciais agora são reproduzidas:
  `python tools/run_probe.py --seconds 55 --press-start --input 1800:CROSS --input 1830:NONE --capture-frames`.
  Os números são quadros da janela do protótipo. `NONE` solta os botões.
  As capturas da sequência ficam somente em `local/probe-frames/`.
- O teste aceita até 300 segundos; comandos `--input` aceitam os quadros 1..18000.
  Para controlar o teste depois de observar uma tela, inicie com `--live-inputs`
  e use, em outro terminal, `python tools/control_probe.py CROSS --frames 30`.
  O comando fica restrito ao controle interno do protótipo e solta o botão
  automaticamente em 1..120 quadros. Não envia teclas ao Windows.
  `python tools/control_probe.py STOP` encerra o teste normalmente, permitindo
  a captura de memória quando `--dump-ram` foi explicitamente habilitado.
- `--dump-ram` salva a memória EE, scratchpad e IOP em `local/` após um
  encerramento normal, inclusive por `control_probe.py STOP`. Também registra
  o estado do IOP. Matar o processo pelo limite de tempo não produz esses dumps.
  As capturas servem à investigação local e não são enviadas ao repositório.
- Funções ausentes interrompem o protótipo; não são ignoradas para simular avanço.
- A integração `sceMpeg` com FFmpeg está habilitada. A entrega dos canais de
  áudio usa a tabela do SDK em `0x306b80`; o cabeçalho privado é conservado para
  o callback original. A reprodução completa ainda não está validada.
  O adaptador separado de `ipu2` usa o decodificador IPU em um processo nativo, valida o quadro
  e grava os pixels em blocos de 16×16 antes de entregar a interrupção ao jogo.
  Esta primeira integração ainda abre o decodificador por quadro; otimização e
  validação de todas as animações continuam pendentes.
- O executável lê a ISO local. Os dados e arquivos gerados do jogo ficam privados.
- Os salvamentos usam `data/mc0`, ao lado dos executáveis, separados do emulador. As ferramentas de teste continuam usando `local/mc0`.

Ainda precisam ser comprovados: equipar habilidades adquiridas, áudio, controle
físico, batalhas, demais recursos gráficos e salvamento/carregamento.
Só depois disso faz sentido medir desempenho e integrar a tradução.

O formato IPU de entrada do adaptador segue o
[demultiplexador do FFmpeg](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/ipudec.c)
e usa seu [decodificador IPU](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/mpeg12dec.c).
Os arquivos temporários com quadros e pixels ficam em `local/native-image/` e
não devem ser incluídos numa distribuição do código do projeto.

## Créditos e distribuição

Preparação e desenvolvimento assistidos por **Codex (OpenAI)**; projeto e testes:
**Seugildinho**. PS2Recomp e as demais dependências mantêm seus créditos e licenças
originais. PS2Recomp usa GPL-3.0; qualquer distribuição de uma compilação derivada
precisa acompanhar seu código correspondente e os avisos aplicáveis.

O checkpoint local inclui adaptadores, ferramentas, mapa de endereços e doze patches.
Não inclui ISO, ELF, recursos do jogo, código recompilado gerado, executáveis ou
dependências. Cada usuário precisa fornecer sua própria cópia do jogo.
O código deste projeto está sob GPL-3.0; veja `LICENSE` e `NOTICES.md`.
