# Estado do protótipo — 23/10/2026

**Ainda não é um port jogável.** A compilação atual passa pelas animações
iniciais e abre título, menu, seleção de histórias, acampamento de Shu, armas
e habilidades. Quinze suítes de regressão passam. A música ainda apresenta
falha de reserva de buffer no IOP.

## Atualizações de 23/10/2026

- **Decodificador IPU2 em processo via libavcodec**: substituído o spawn
  síncrono de processo externo `ffmpeg.exe` por decodificação em processo
  (`src/native_ipu_decoder.cpp`, `AV_CODEC_ID_IPU` via FFmpeg 7.1). O tempo
  de decodificação nos menus caiu de ~58 ms/quadro para ~0,16 ms.
- **Cache de field buffer duplo**: o jogo decodifica o mesmo quadro para
  os fields par e ímpar consecutivos; o cache em `src/native_image.cpp` atende
  50% dos quadros instantaneamente.
- **Dumps de diagnóstico sob demanda**: a exportação periódica de `boot-frame.png`
  e `gpu-vram.bin` agora exige `OROCHI_DIAG_DUMPS`, eliminando o engasgo de 25 ms
  a cada 2 segundos no jogo manual.
- **Backend D3D11 Compute parcial**: integrado `src/native_gpu.cpp`, ativado
  apenas com `OROCHI_RENDERER=d3d11`. O iniciador normal usa CPU. A GPU atende
  sprites de cor constante; texturas e triângulos continuam na CPU. Não há
  garantia de ganho com esse caminho misto, que exige transferências de VRAM.
- **15 suítes de regressão aprovadas**: integrados os testes `--test-gpu` e
  `orochi_ipu_check`, mantendo 100% de aprovação no executável atual
  (SHA-256 `a972ca58d0084c17d4922c7856673ac0cfada032b247a8dfd65fc8f9eef1ce83`).

## Estado anterior (22/10/2026)

**Ainda não é um port jogável.** A compilação atual passa pelas animações
iniciais e abre título, menu, seleção de histórias, acampamento de Shu, armas
e habilidades. O teste de 291,20 segundos foi encerrado normalmente; as treze
suítes passam. A música ainda apresenta falha de reserva de buffer no IOP.

Missão de Ueda, dificuldade e apresentação do capítulo foram vistas em builds
anteriores; esses três resultados ainda precisam ser repetidos na versão atual.

O repositório permanece privado. A tradução PT-BR é outro projeto e não foi
alterada nesta etapa. O protótipo usa a ISO original USA indicada no README.
Por solicitação do responsável, as mudanças posteriores ao checkpoint do
acampamento estão sendo salvas apenas no PC, sem atualizações no GitHub.

## Histórico anterior às correções de memória e MPEG

- Corrigida a ABI de scePadRead: o SDK do jogo espera 32 bytes lidos; o serviço
  genérico retornava 1. Leituras que falham continuam retornando falha.
- O teste executa o próprio chamador recompilado do jogo para comprovar a
  aceitação do pacote; também verifica porta fechada e limites da memória.
- START abre o menu e CROSS confirma História após terminar sua animação.
- Os comandos dos testes são injetados somente no serviço interno do protótipo.
- A abertura do menu é lenta nesta versão: os primeiros comandos de confirmação
  chegavam durante a animação e eram ignorados pelo próprio jogo.
- Identificado o módulo de código ovl_00.bin, que o jogo lê do disco para a
  transição seguinte. Seu conteúdo na memória corresponde exatamente à ISO.
- Extração verificada por SHA-256, análise de 166 intervalos do módulo e geração
  de um ELF privado adicional usado somente para produzir o C++.
- O protótipo continua carregando o ELF original. O carregador original faz a
  leitura e a inicialização do módulo; o adaptador verifica sua impressão digital
  antes de liberar a execução do código recompilado correspondente.
- Recuperadas nove chamadas do módulo para rotinas do executável principal que
  não eram conhecidas na primeira análise.
- Após essas correções, a seleção de histórias apareceu na janela nativa.
- Teste de 120 segundos: a escolha de Shu abriu o acampamento, com Zhao Yun,
  Xing Cai e Yoshihiro Shimazu. Os rótulos de armas e habilidades aparecem;
  ainda não entramos nesses dois menus nem numa batalha.
- A navegação TUTORIAL → SAVE → NEXT abre a seleção de cenários. A confirmação
  de Ueda abre a dificuldade, e NORMAL abre a apresentação do capítulo.
- Ao encerrar essa apresentação com TRIANGLE, o primeiro teste encontrou uma
  entrada ausente em 0x270e00, chamada por JR em 0x270ddc. A tabela original
  em 0x3387b0 contém oito opções, com três destinos distintos.
- O código desses destinos já estava decodificado em uma região descontínua.
  A geração agora separa as entradas comprovadas em intervalos próprios,
  conservando os limites e as instruções originais. A tabela e a evidência
  estão em config/analysis.json; não foi criado retorno fictício.
- A repetição ultrapassou o salto corrigido e encontrou JALR 0x1ad1e0 →
  0x236020 durante o carregamento seguinte. A tabela em 0x320fe0 tem 11
  registros de oito bytes, com callback e argumento separados. A análise
  recuperou a função real 0x236020..0x2360dc e a compilação foi refeita.
- O teste seguinte passou por esse callback e encontrou a inicialização dos
  objetos: JALR 0x266914 → 0x27bb80, registro 31 em 0x337dc0.
  O código original define uma tabela de 42 registros de 48 bytes em
  0x3377f0..0x337fd0. Seus campos de callback foram separados das flags
  numéricas e das entradas nulas, recuperando mais 17 funções reais.
- Nove suítes de regressão aprovadas: DMA, VSync, GIF/VIF, exibição, controle,
  proteção dos módulos, tabela de saltos, comunicação SIF e memória do IOP. A tabela de saltos
  executa 16 caminhos reais.
- Corrigida a integração de sceSifInitCmd: o serviço nativo não instalava os
  handlers de sistema nem a estrutura de registradores na memória do jogo.
  Os pacotes reais do IOP agora passam pelos handlers originais, incluindo
  SetSreg, e atualizam a região que o leitor de estado do som consulta.
  O teste executa esse leitor original e verifica que reinicializar conserva
  os valores recebidos. A execução completa confirmou a chegada dos pacotes.
- A captura da tela preta localizou uma espera em 0x2a5e38 pela rotina
  0x19b330. O pedido de música é consumido, mas o driver original do IOP
  responde -10 ao comando 0x96: seu buffer de reprodução não está configurado.
- Corrigido um defeito independente confirmado no alocador do IOP: blocos
  liberados eram ignorados pelas novas alocações. O patch 0006 reutiliza esses
  espaços, conserva blocos ativos e distingue maior bloco livre de total livre.
  O teste executa 100 ciclos com o tamanho real do buffer musical, 0x52000 bytes.
  Uma comparação isolada compila a versão original e a corrigida: a original
  falha no terceiro ciclo; a corrigida completa os 100.
- Repetição completa de 248,64 segundos, encerrada normalmente por STOP:
  os menus e a apresentação continuam funcionando, sem nova função ausente.
  A tela preta persiste na mesma espera. O buffer do IOP chegou a ser configurado
  (endereço auxiliar 0x1d8f00 e tamanho de bloco 0x800), mas sua variável principal
  está zero ao final. A rotina original 0x3754, comando 0x93, libera esse buffer
  e zera apenas essa variável. Ainda falta rastrear a sequência de configuração
  e liberação para explicar sua ausência quando chega o comando de música.
- O carregador recusa módulos não recompilados antes de sobrescrever a região
  compartilhada ou chamar seus construtores. O teste comprova a preservação
  da memória, do estado do carregador e dos registradores nesse caso.
- Os testes aceitam 300 segundos, comandos programados até o quadro 18000 e
  comandos internos observados em tempo real, com liberação automática.

## Memória do IOP e seletores de áudio — checkpoint anterior

- O rastreamento limitado do RPC de som comprovou a causa da falha de configuração:
  o driver pediu 0x52000 bytes quando o maior bloco livre tinha apenas 0x30100.
  O comando 0x92 retornava -3. A reutilização de blocos, isoladamente, não bastava.
- O patch 0008 faz os módulos e os buffers compartilharem uma única reserva na
  RAM real de 2 MiB. Conserva os primeiros 64 KiB e a pilha nativa superior,
  protege código/BSS e devolve a reserva quando o módulo é descarregado.
  Imagens inválidas ou sobrepostas são rejeitadas sem alterar blocos ativos.
- O teste do carregador passa; as quatro suítes de compatibilidade do ps2xIOP
  também passam. No jogo, os oito módulos carregam e os comandos de configuração
  de áudio passam a retornar sucesso. As bases relocadas dos módulos mudaram.
- FFmpeg MPEG-2 foi habilitado. O pacote 7.1 para Windows é fixado por SHA-256
  no patch 0009; o adaptador IPU externo continua usando FFmpeg 8.1 separadamente.
- O patch 0010 usa o SMODE2 efetivamente programado pelo jogo em sceGsSyncV.
  O teste executa a espera original de troca de campo, partindo de ambos os
  campos, e conserva o retorno do modo progressivo.
- O SDK original registra áudio com os seletores 3 e 4. O serviço genérico
  entregava tipos 1 e 2 e não filtrava o número do canal. O patch 0011 usa os
  padrões e máscaras da tabela original 0x306b80 e conserva o cabeçalho privado
  de quatro bytes, que o próprio callback remove.
- O teste de áudio verifica os seletores, canal diferente, PES dividido e a
  cópia do cabeçalho pelo callback original recompilado 0x115008.
- Teste real encerrado normalmente por STOP em 119,95 segundos: 149 quadros
  640×448 consumidos; fila de áudio inicializada; 0x40020 bytes transferidos.
  Nenhuma nova função ausente ou exceção do processo. A captura final fica preta.
- O bloqueio mudou: produtor em 0x114358, consultando 0x116c68/0x116b48;
  a fila de entrada ainda conta 0x7e800 bytes. O decodificador espera outro quadro
  em 0x116eb4, com ended=0. Investigar o consumo dessa fila e o fim de sequência
  na integração entre SDK recompilado e decodificador nativo, além da saída gráfica.
- Os onze patches foram aplicados, em ordem, a cópias limpas dos arquivos do
  commit fixado. Os 20 arquivos resultantes são idênticos aos usados na compilação;
  evidência em relatorios/patches.json. As doze suítes passam no mesmo executável
  usado no teste real, SHA-256 c9ba1d3d734ba4c763b3bb7dce2c69a22257f154db635a2e14621823b4841ba6.

## Encerramento nativo do vídeo e menus — checkpoint atual

- A revisão das capturas intermediárias comprovou que o logotipo KOEI já era
  exibido no checkpoint anterior. A captura final preta representava a espera
  após a animação; não comprovava falha no formato dos pixels. O construtor
  original 0x1175f8 usa blocos em ordem de colunas, compatível com o formato atual.
- Identificada a duplicação do caminho de entrada: DemuxPss alimentava FFmpeg
  e chamava 0x114ef0, que também enfileirava os mesmos bytes no IPU físico.
  GetPicture nativo não consumia essa segunda fila; ela acumulava 0x7e800 bytes.
- O patch 0012 registra recibos somente depois da entrega dos bytes ao
  decodificador. src/native_mpeg.cpp confirma endereço/tamanho/instância exatos
  no callback do jogo e dispensa a segunda cópia física. Recibos inválidos ou
  repetidos falham; não são alterados os contadores da fila antiga.
- A rotina original 0x116b98 é o pedido explícito de fim de entrada. Seu
  adaptador aguarda os recibos, finaliza o decodificador e acorda o leitor.
  As consultas pequenas do SDK 0x14b0c8/0x14b0d8 passam a consultar o estado
  nativo. O fim só é informado após esvaziar os quadros e seu tempo de apresentação.
- Treze suítes passam. A nova suíte cobre recibos incorretos, outra instância,
  repetição, confirmação pendente no EOF, quadros pendentes e apresentação,
  além de verificar que o buffer físico não foi modificado.
- Teste real de 291,20 segundos no mesmo executável, SHA-256
  901d49b67b7be2db4cb8f3eefc4dda2d03007c6b84bd5f381326f59cea8916c8:
  logotipos KOEI/Omega Force, título, menu pelo START, História, Shu e acampamento.
  O módulo ovl_00 foi verificado. Nenhuma função ausente ou erro fatal.
- Armas: aberto o painel de Zhao Yun, Bronze Spear, imagem da lança e ataque 10.
  Habilidades: tela aberta com os espaços iniciais vazios; equipar habilidades
  adquiridas ainda não foi testado. Não houve batalha nem gravação de save.
- Permanece uma falha na configuração musical após as animações: pedido de
  0x52000 bytes, maior bloco livre 0x43670, total livre 0x6d8f0; RPC 0x92 retorna
  -3. Os menus ainda navegam. Investigar as reservas e liberações que fragmentam
  a memória antes de repetir Ueda. Não aumentar a RAM nem forçar o estado de som.
- Doze patches aplicados a arquivos limpos reproduzem os 21 arquivos usados
  na compilação. O histórico e as evidências permanecem locais.

## Compilação e limites

Mapa atual: 5.922 intervalos principais e 166 do módulo, acrescidos de duas
divisões de entradas da tabela de saltos, sem sobreposição.
O PS2Recomp processa 6.090 intervalos, com 5.871 recompilados e 219 serviços
ligados à infraestrutura. Zero falhas de decodificação, instruções sem tradução
ou erros de geração; os 678 avisos existentes continuam exigindo análise.

A renderização GS é feita na CPU e a janela usa OpenGL/raylib. **Sem Vulkan.**
O EE é recompilado para x64; IOP e VU ainda dependem dos serviços e intérpretes
da infraestrutura. O adaptador IPU abre FFmpeg por quadro e continua experimental.

Não estão validados: batalhas, áudio, controle físico, salvamentos e desempenho.
O encerramento da primeira animação foi corrigido e os menus voltaram a abrir.
A falha conhecida atual é a reserva do buffer musical por fragmentação do IOP.
A espera após Ueda ainda precisa ser repetida. Não foram forçados registradores,
contadores de fila ou resultados de sucesso para contornar esperas.
Os dois módulos do disco (ovl_00.bin e ovl_01.bin, este com OPTIONS e GALLERY) estão
integrados; dividem o endereço 0x819000 e a tabela de funções é trocada a cada carga.
Outro módulo deve parar antes de ser carregado, com uma indicação explícita.
Não se deve ignorar funções ausentes ou tratar ausência de erro como prova de jogo funcional.

## Reprodução e evidências

Veja README.md e RETOMADA.md. VALIDACAO.json identifica a compilação e os testes.
As capturas locais incluem relatorios/menu-principal.png e
relatorios/selecao-historia.png e relatorios/acampamento-shu.png.
As novas capturas incluem selecao-cenario-shu.png, dificuldade-shu.png e
introducao-ueda.png. Ainda não comprovam a batalha.
Sequências completas ficam em local/probe-frames/.
O teste mais recente e os registros completos estão em relatorios/ e logs/.
Rodadas anteriores estão preservadas em local/tests/.

ISO, ELF, módulos binários, memória capturada, imagens, código derivado e
executáveis continuam somente no PC. O backup contém o trabalho e as instruções
para reconstruí-lo a partir da cópia original do jogo.
