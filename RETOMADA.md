# Retomada do desenvolvimento

Checkpoint de 23/10/2026. Atualizações locais conforme solicitado; backup remoto pausado.
A tradução PT-BR é outro projeto.

## Checkpoint de 23/10/2026: Diagnóstico de lentidão e decodificador IPU2 em memória

### 1. Melhoria parcial do decodificador IPU2 e dos diagnósticos
- **Causa Raiz Medida**: Na navegação dos menus (Press Start, Menu Principal, História, Acampamento), o jogo decodifica continuamente o vídeo de fundo `ipu2` a 1 quadro por VBlank (duas vezes por seleção, alternando fields par/ímpar). O caminho legado gravava `local/native-image/frame.ipu` em disco, executava `CreateProcessW("ffmpeg.exe")` a cada quadro (~61,2 ms/quadro na GameThread do EE) e lia `frame.rgba` do disco, limitando o jogo a ~5,5 FPS / 11 VBlanks a cada 2s.
- **Implementação do Decodificador em Processo**: Criados `src/native_ipu_decoder.h` e `src/native_ipu_decoder.cpp`, utilizando a `libavcodec` já vinculada ao runtime (`AV_CODEC_ID_IPU` via FFmpeg 7.1) e conversão de cores direta em memória (`swscale`).
- **Cache de Fields Duplos**: Adicionado cache do último quadro decodificado em `src/native_image.cpp` para evitar re-decodificar o mesmo payload de imagem quando submetido para os buffers de field par e ímpar consecutivos.
- **Resultados Medidos**:
  - O tempo no decodificador caiu de ~58 ms/quadro para ~0,16 ms por quadro no processo (ganho de mais de 300x).
  - 50% dos quadros são atendidos instantaneamente pelo cache de field (`ipuCacheHits`).
  - O tempo total de IPU em 2 segundos caiu de 438–534 ms para 34–39 ms.
- **Validação de Equivalência Rígida**:
  - Teste de regressão dedicado `orochi_ipu_check.exe` (`tools/ipu_check_main.cpp`): 50 iterações com 0 bytes de diferença (`diffBytes=0`, `maxDelta=0`, média de 0,311 ms).
  - Teste ao vivo no jogo com `--ipu-verify 80`: 68 quadros decodificados em tempo de execução comparados byte a byte contra o CLI do FFmpeg: 68/68 idênticos (`identical=68, different=0`).
- **Desacoplamento de Dumps de Diagnóstico**:
  - A exportação periódica de `relatorios/boot-frame.png` e `gpu-vram.bin` em disco agora só ocorre se `OROCHI_DIAG_DUMPS` estiver definido (ativado em `tools/run_probe.py`), poupando 21–31 ms a cada 120 quadros em execuções normais via `Abrir port.cmd`.

### 2. Integração do Backend D3D11 Compute e 15 Suítes de Regressão
- Integrado o backend Direct3D 11 parcial (`src/native_gpu.cpp`), ativado somente com `OROCHI_RENDERER=d3d11`. O iniciador normal usa CPU. Somente sprites de cor constante são acelerados; texturas e triângulos usam CPU e ainda exigem transferências entre processador e GPU.
- Adicionadas as suítes `--test-gpu` e `orochi_ipu_check` ao `tools/run_regressions.py`.
- **15/15 suítes de regressão 100% aprovadas** no executável `WarriorsOrochi-native-prototype.exe` (SHA-256 `a972ca58d0084c17d4922c7856673ac0cfada032b247a8dfd65fc8f9eef1ce83`, 209.068.032 bytes).

## Checkpoint de 22/10/2026. Envio deste checkpoint ao GitHub autorizado pelo usuário.
Continuar o desenvolvimento local; novos envios dependem de outro pedido.
O backup remoto permanece privado. A tradução PT-BR é outro projeto.

## Teste manual posterior: transição e lentidão

No teste do usuário acompanhado em 22/10/2026, o acampamento respondeu aos
comandos. Após selecionar NEXT houve fade, tela preta e encerramento. O log
mostra chamada indireta inválida de 0x117a14 para 0x53000000. Não interpretar
isso como apenas a falha antiga do buffer musical: a causa desta chamada ainda
precisa ser rastreada. Combate não observado. Evidência preservada em
local/teste-usuario-20261022-230426/.

O usuário também relatou muita lentidão. Conferência do código: GS usa
GSCpuBackend (renderização por CPU); OpenGL apresenta a imagem na janela.
O limite da janela é 60 FPS, mas isso não comprova a velocidade do jogo.
Diagnósticos ainda exportam PNG a cada 120 quadros, inclusive no iniciador
manual. Medir tempos de renderização, execução EE/IOP e diagnóstico antes de
atribuir o gargalo ou declarar melhoria. Ainda não houve perfil de desempenho,
otimização ou migração para Vulkan neste teste. Não culpar o hardware do usuário.

## Estado atual

13 suítes passam no executável 901d49b67b7be2db4cb8f3eefc4dda2d03007c6b84bd5f381326f59cea8916c8.
Teste real de 291,20 segundos, encerrado normalmente por STOP, com oito módulos
IOP e ovl_00 verificado. A abertura passou pelo bloqueio antigo e mostrou KOEI,
Omega Force e título. START abriu o menu, História abriu Story Select e Shu.

Armas: aberto o detalhe de Zhao Yun, Bronze Spear, imagem e ataque 10.
Habilidades: tela aberta com espaços iniciais vazios. Equipar habilidades
adquiridas, batalhas, áudio audível, controle físico e saves NÃO comprovados.
Ueda/dificuldade/apresentação foram vistos em builds anteriores e precisam
ser repetidos no atual. Todas as imagens atuais foram inspecionadas.
Evidências e hashes em relatorios/mpeg_transport_progress.json.

## Próximo trabalho: fragmentação do buffer musical do IOP

Os menus ainda navegam, mas após as animações o driver volta a falhar ao
configurar música. Não assumir que a primeira correção do pool resolveu todas
as transições:

- AllocSysMemory modo 0 pede 0x52000 bytes.
- Maior bloco livre 0x43670; total livre 0x6d8f0.
- RPC SID 0x4b534e47, comando 0x92: argumentos 0x5010, 1, 0xa1; resultado -3.
- Comandos posteriores 0x96 com todos os argumentos -1 retornam -10; distinguir
  solicitações de parar música de uma solicitação efetiva de reprodução.
- Última falha: caller IOP 0x72a8c; confirmar a base relocada do driver a cada teste.

Rastrear reservas/liberações vivas ao atravessar KOEI/Omega Force e a animação
seguinte. A memória tem espaço total suficiente, porém fragmentado. Não ampliar
os 2 MiB, não mover blocos ativos arbitrariamente, não forçar registradores de
som e não declarar reprodução bem-sucedida sem dados reais.

Locais: vendor/PS2Recomp/ps2xIOP/src/emulator/core/iop_memory.*, imports/iop_sysmem.cpp,
services/iop_module_loader.cpp e services/iop_rpc.cpp. A versão atual de sysmem
trata modo 1 como modo 0; investigar a semântica e os usos reais antes de mudar.
Os logs de RPC são limitados por comando; --dump-ram habilita o diagnóstico.
A rotina original de configuração é IRX 0x37d8; 0x3754 libera o buffer.
A base do driver mudou com o pool conjunto; não usar a antiga base 0x53200.

## Correção do vídeo que deve ser preservada

O checkpoint anterior decodificava 149 quadros, mostrava KOEI e parava ao final.
A fila física permanecia com 0x7e800 bytes, pois DemuxPss alimentava FFmpeg e
0x114ef0 também copiava o mesmo vídeo para o IPU que GetPicture nativo não lia.
O formato dos pixels já estava correto: 0x1175f8 monta blocos em ordem de colunas.
Não trocar por blocos em ordem de linhas nem tirar conclusões só da captura final.

src/native_mpeg.cpp e patch 0012:

- Demux emite recibo após entregar o ES ao decodificador nativo.
- 0x114ef0 confirma instância/endereço/tamanho exatos e dispensa a segunda cópia
  no IPU físico. Confirmações desconhecidas ou repetidas falham.
- 0x116b98 é o pedido explícito do produtor para encerrar entrada; aguarda recibos
  pendentes, finaliza FFmpeg e acorda o leitor. É por instância, sem definir EOF
  do disco inteiro. Não basta observar sequence_end isoladamente.
- 0x14b0c8 e 0x14b0d8 consultam fim de reprodução e fila de referência nativos.
- O fim aguarda os quadros e seu tempo de apresentação; a fila física não é zerada.
- --test-mpeg-transport cobre recibos, EOF e quadros pendentes, apresentação e
  preservação do buffer físico. Todos os outros doze testes continuam passando.

O áudio usa os seletores originais 3 e 4, tabela 0x306b80. Conservar o cabeçalho
privado de quatro bytes: o próprio callback original o remove. FFmpeg 7.1 é
usado por MPEG; CLI 8.1 separado é usado por IPU2.

## Repetir e observar

```powershell
python tools/workflow.py native
python tools/run_regressions.py
python tools/run_probe.py --seconds 300 --press-start --capture-frames --dump-ram --live-inputs
```

START programado no quadro 600 pode cair durante os logotipos. Esperar o título
e usar controle interno observado, sem mandar teclas ao Windows:

```powershell
python tools/control_probe.py START --frames 30
python tools/control_probe.py CROSS --frames 30
python tools/control_probe.py STOP
```

Parar normalmente ANTES de 280 segundos para deixar margem ao dump. A última
rodada encerrou em 291,20; o timeout de 300 segundos mata o processo e não salva
a memória. Botões por apenas um quadro podem não ser percebidos; usar 30 quadros
e deixar pelo menos 1,2 s entre pulsos para o jogo notar a soltura.

Capturas atuais: local/probe-frames/20261022-202040; quadros 4800 (título),
7800 (menu), 9360 (história), 12000 (camp), 14880 (arma), 16320 (habilidades).
Cópias com hashes em relatorios/*-native-20261022.png.
Memórias e contexto atuais em local/*-after-stop.*, logs em logs/boot.*.log.
Rodada anterior em local/tests/before-native-mpeg-transport-20261022-201830.

```powershell
python tools/inspect_function.py 0x116b98
python tools/inspect_function.py --iop 0x37d8
```

As leituras usam os projetos Ghidra existentes sem modificá-los. O driver
analisado usa endereços antes da relocação, projeto OrochiIop, arquivo privado
local/iop-staging/module-1179648.irx. Decompilados em local/analysis/decompiled/.

## Reprodução e limites

12 patches aplicados a cópias limpas reproduzem os 21 arquivos compilados;
relatorios/patches.json. As quatro suítes originais do IOP passam e seu código
não mudou nesta etapa. O histórico detalhado fica em PROGRESSO.md.

Mapa: 5.922 intervalos principais + 166 de ovl_00 + duas divisões = 6.090;
5.871 recompilados, 219 serviços, zero erros, 678 avisos de geração.
Mapas em config/orochi.ranges.csv e config/overlay_00.ranges.csv.
OvL_00: 142.400 bytes, ISO 0x37c000, base 0x819000, entrada 0x83a810;
fingerprint em config/overlay_00.json.
ovl_01 (OPTIONS/GALLERY): 48.256 bytes, ISO 0x39f800, mesma base; geração parcial
própria em tools/generate_supplemental.py (overlay_module), símbolos ovl01_*,
fontes generated/zzz_ovl01_*, mapa config/overlay_01.ranges.csv. Como o endereço é
o mesmo, src/native_overlay.cpp troca as entradas da tabela após validar o módulo
carregado. Módulos não implementados param antes de substituir o código existente. Novos alvos exigem evidência e atualização
do mapa; não ignorar chamadas ausentes.

ISO USA original na pasta superior; ELF em local/SLUS_216.62.
GS na CPU, janela OpenGL/raylib, sem Vulkan. IOP/VU ainda usam intérpretes.
Não incluir ISO, ELF, IRXs, RAM, imagens, generated/, vendor/ ou binários no
backup de fontes. Ainda não há versão jogável ou tradução integrada ao port.
