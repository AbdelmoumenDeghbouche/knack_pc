# Linhas pretas nas RX 6600 (RDNA2 de 8 GB, Navi 23)

Diagnóstico de 2026-10-12 e 2026-10-13, feito por leitura do código, dos relatos e dos logs, mais
quatro builds de teste rodados por quem tem a placa. Nenhuma Navi 23 está disponível localmente
(a máquina de desenvolvimento tem uma GTX 1650, a de teste uma RX 6700 XT, Navi 22, que não
mostra o defeito). A corrupção na Intel Arc está em `2026-10-12-gpu-corruption-intel.md`.

Issues: [#65](https://github.com/FelipePrado0/gow3_pc/issues/65) (canônica),
[#61](https://github.com/FelipePrado0/gow3_pc/issues/61),
[#43](https://github.com/FelipePrado0/gow3_pc/issues/43),
[#27](https://github.com/FelipePrado0/gow3_pc/issues/27); shadPS4 eltutz:
[cuesta4/shadPS4#8](https://github.com/cuesta4/shadPS4/issues/8).

## 1. Sintoma

| | AMD RDNA2 de 8 GB |
|---|---|
| Placas | RX 6600, 6600M, 6600 XT, 6650 XT (todas Navi 23). 6700 XT (Navi 22) limpa. |
| Drivers | Adrenalin 26.6.4 e 26.8.1 (Vulkan 2.0.353), com `VK_KHR_pipeline_binary` e `VK_EXT_subgroup_size_control`. |
| Aparência | Linhas pretas finas em blocos retangulares, só em áreas claras (céu, fogo, brilho do HUD, pele do Kratos no menu, fontes de luz e brilhos especulares; nunca nas sombras). Medido no print 1920x1080 de #61: blocos de 8x4 px alinhados ao grid de resolução cheia. Em 4K nativo as faixas ficam quatro vezes menores na tela e se concentram na parte de baixo. |
| Comportamento | Aleatório por lançamento (quase sempre aparece), estático durante a execução, no mesmo lugar da cena. |
| Em outros builds | Igual no shadPS4 eltutz **e no shadPS4 0.19.0 oficial** (confirmado na 6650 XT em 2026-10-13). Não é regressão do fork. |

## 2. Fatos apurados

- O quadro do jogo (docs/upscaler.md): pré-passe de profundidade, luz e sombras, materiais,
  transparências, pós com pirâmide de bloom em meia resolução, combine da pós em **compute**
  (hash `9a9cf8a9`) para a cor da cena RGBA8 sRGB, HUD por cima na mesma imagem.
- `Physical device subgroup size 64 (32 to 64, pinnable stages { Compute })` (build de teste 1,
  RX 6600): o driver AMD só aceita `requiredSubgroupSize` em compute. O pin de 64 nos estágios
  gráficos, herdado do shadPS4, é inválido na AMD; o driver escolhe o wave size do pixel shader
  sozinho. Uma wave32 de PS cobre 8x4 px, a geometria medida nos prints. **Testado e descartado
  como causa** (seção 4).
- `VkPipelineExecutablePropertiesKHR::subgroupSize` vem 0 para todo executável gráfico na AMD
  (Windows), 64 só em compute: o histograma de `GOW3_WAVE_STATS=1` é cego para vertex e
  fragment nessa placa. `VK_AMD_shader_info` (build 3) devolve estatísticas e ISA por estágio.
- Nenhum fragment shader depende do tamanho da wave: varredura de 1152 módulos SPIR-V do cache
  (jleandrocampos): 76 `OpGroupNonUniformQuadBroadcast` (quad, independe da wave) e 4 ballots,
  todos em compute.
- O eltutz tem 104 commits próprios sobre a base de 2026-08-21 (256 atrás do main). O fork
  vendora o upstream `b4e7ae7` (2026-10-02, a data do 0.19.0) mais três correções do eltutz:
  alias de imagens (`189ca3b6`), depth sem uso não anexado (`eccbeae3`) e blend MIN/MAX com
  escala, com um "square pass" (`45f3d1f9`, chave `GOW3_SQUARE_PASS=0`).
- Limiares do coletor de VRAM numa placa discreta de 8 GB (`texture_cache.cpp`, construtor):
  `trigger_gc_memory = 0` (o coletor roda a cada submissão), pressão em cerca de 2,7 GB e
  crítico em cerca de 5,4 GB. Nos 12 GB: gatilho cerca de 1,35 GB, pressão 5,9 GB. Imagens
  tiled escritas pela GPU nunca são despejadas; alvos por frame nunca entram na LRU.
- Oito logs lidos por inteiro (seis reporters): execuções saudáveis, sem erro, sem pressão de
  memória, Fifo e Immediate, janela e tela cheia, quatro builds de eboot diferentes. Nenhuma
  linha se correlaciona com o defeito.
- O pacote de release não inclui cache de shaders; cada reporter compila o próprio.
- O port só grava linhas de GPU em nível Warning no `user\last_run.log`
  (`gpu/shim/common/logging/log.cpp`); `GOW3_GPU_LOG=info` libera. Marcadores para testers
  precisam ser `std::printf("GPU: ...")`. Até o build 3 só o `Play God of War III.exe`
  (`--play`) gravava o arquivo; o botão PLAY da janela mandava a saída para a aba Log. Corrigido
  nesta PR.

## 3. Hipóteses refutadas por leitura (2026-10-12)

Nenhuma sobreviveu à verificação adversarial. Motivos, para não repetir o trabalho:

| Hipótese | Por que caiu |
|---|---|
| Draws pulados pela compilação assíncrona deixam alvos indefinidos | Imagens novas nascem `Dirty` e passam por `RefreshImage` antes do primeiro uso; alvos de bloom são refeitos a cada frame; só existe no fork, não no eltutz; o primeiro uso de um pipeline é pulado em toda máquina, inclusive na 6700 XT. |
| Coletor de VRAM em 8 GB | A conta está certa, mas alvos tiled escritos pela GPU nunca são despejados, só imagens paradas há 16 a 160 ticks entram, imagens recriadas voltam da memória do guest, e o sintoma aparece no menu com pouca memória em uso. |
| VMA derramando para RAM | Só custa desempenho. Endurecimento: passar `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` em `vk_instance.cpp` (a extensão está ligada, a flag não). |
| Cópia de alias (`aliasing.cpp`) ou memo com imagem velha | Cópias de extensão inteira a partir da origem, só com layout igual. Não geram blocos de 8x4. |
| Memos, draw pipe, gravação em thread | Determinísticos e iguais na 6700 XT. Lacuna real confirmada, mas de outro efeito: uma meta-clear via compute ou `FillBuffer` com o render pass aberto e acerto no `begin_memo` pula `IsMetaCleared/TouchMeta` (alvo inteiro sem clear, não linhas). |
| Chave do cache de binários | `CreateCachedGraphicsPipeline` recebe a `GraphicsPipelineKey` e não a usa; o arquivo leva só a chave do driver (`vk_pipeline_serialization.cpp`). Efeito seria determinístico, na 6700 XT também, e não existe no eltutz. Endurecimento: incluir o hash da chave no nome e remover `LoadLegacyBinary`. |
| Toggle de binários sem efeito | Verdadeiro só sem `VK_KHR_pipeline_binary`; o log da RX 6600 tem a extensão. |
| Combine `9a9cf8a9` em wave32 | Todo compute passa pelo mesmo caminho com `requiredSubgroupSize=64`; compute não usa binários. |

## 4. Testes em hardware (2026-10-13)

**Bisseção nas otimizações do port (FilipiEPTeixeira, RX 6600, #65): os 7 testes falharam**, 5
aberturas cada, linhas em todas. Um de cada vez, cache de shaders limpo antes:

| # | Teste | Como | Resultado |
|---|---|---|---|
| 1 | Sem cache de binários e sem carga em segundo plano | desmarcar "Use pipeline binary cache" e "Load shader cache in the background" | linhas |
| 2 | Sem compilação em segundo plano | desmarcar "Compile new shaders in the background" | linhas |
| 3 | Sem o square pass do MIN/MAX | `GOW3_SQUARE_PASS=0` | linhas |
| 4 | Sem o coletor de VRAM | `GOW3_GC_BUDGET_MB=20000` | linhas |
| 5 | Draw pipe desligado | "Pipeline de GPU em dois estágios" = Off | linhas |
| 6 | Gravação na thread principal | `GOW3_VK_RECORD_THREAD=0` | linhas |
| 7 | Leituras com espera | desmarcar "Game reads GPU data without waiting" e "Deferred GPU readbacks" | linhas |

**Pin de subgrupo (build de teste 2, `windows-v2.1.6-test2`):** três execuções com cache limpo,
(1) política normal, (2) `GOW3_SUBGROUP_PIN=allowed`, (3) `GOW3_SUBGROUP_PIN=none` com
`GOW3_LDS_BARRIERS=1`. MarcosAlexandre30 (RX 6600): linhas nas três. jleandrocampos (RX 6650 XT):
linhas na 1 e na 3, com o log confirmando a política aplicada (`compute pinned to 64, fragment
pinned to 64` contra `compute pinned to 0, fragment pinned to 0, LDS barriers on`). **O pin não
é a causa.** As variáveis ficaram na PR Intel; esta PR não as traz.

**42 toggles desligados ao vivo** (`GOW3_TOGGLE_FILE` = 182395781688328191) e
`GOW3_SQUARE_PASS=0` (jleandrocampos): linhas continuam.

**shadPS4 0.19.0 oficial** na 6650 XT, mesma cena: linhas. Não há regressão do eltutz nem do
fork a procurar; a causa está no núcleo compartilhado ou no driver.

**Build de teste 3 (`windows-v2.1.7-test3`)** troca a medição da largura das waves por
`VK_AMD_shader_info` (ISA em `user\shaders\wave_dumps`). **Build de teste 4
(`windows-v2.1.7-test4`)** acrescenta `GOW3_FULL_BARRIERS=1`. Sem resultado dos reporters até
2026-10-13 02:20 UTC; Ed1z19 (#61) confirmou que o build 3 com configurações padrão mantém as
linhas no mesmo lugar.

## 5. O que esta PR entrega

Nenhuma correção: diagnóstico. Nada muda para quem não liga as variáveis.

- `GOW3_WAVE_STATS=1` liga `VK_KHR_pipeline_executable_properties` e, na AMD,
  `VK_AMD_shader_info`; grava `GPU: wave sizes after N pipelines: { Fragment }@W/vgprV xN, ...`
  (largura de subgrupo e VGPRs físicos por estágio) e a ISA dos três primeiros shaders de cada
  estágio em `user\shaders\wave_dumps\*.txt`, onde wave32 e wave64 aparecem direto
  (`vk_wave_stats.h`, `vk_pipeline_common.cpp`, teste `tests/test_wave_stats.cpp`, alvo
  `wave-stats-test`).
- `GOW3_FULL_BARRIERS=1` grava uma barreira completa de memória antes e depois de cada render
  pass e de cada dispatch de compute (`vk_scheduler.cpp`, `vk_rasterizer.cpp`). Lento; só para
  este teste: linhas sumindo numa cena que as tinha = barreira faltando no núcleo; linhas iguais
  = sincronização sai da lista.
- O botão **PLAY** do launcher grava `user\last_run.log` como o `Play God of War III.exe`
  (`launcher/gow3_launcher_win.py`, teste `tests/test_launcher_run_log.py`). Até o build 3 só o
  segundo gravava, por isso tantos reporters não achavam o arquivo.

## 6. Pistas restantes

1. **Bloco de 8x4 px em RGBA8 = 128 bytes = o bloco de compressão DCC.** O driver AMD no
   Windows não expõe `VK_EXT_image_compression_control`, então não dá para desligar a DCC por
   imagem no Vulkan. Explicaria o alinhamento ao grid e a restrição à Navi 23 (Infinity Cache
   de 32 MB contra 96 MB na Navi 22), não a aleatoriedade por lançamento.
2. **Barreira faltando entre uma escrita única (LUT ou textura gerada por shader) e a primeira
   leitura.** Explicaria estático por execução, aleatório por lançamento, dependente de timing e
   cache e restrito à iluminação. Teste: `GOW3_FULL_BARRIERS=1` (build 4) e validação de
   sincronização (`GOW3_VK_VALIDATION=1 GOW3_VK_VALIDATION_SYNC=1`, exige o Vulkan SDK).
3. **Primeiro passe com blocos 8x4 numa captura RenderDoc** (estágio e wave size): decide entre
   pixel shader, compute e cópia.

### Endurecimentos opcionais (não são a causa)

- Invalidar o `begin_memo` em `TextureCache::ClearMeta` e em `TouchMeta(is_clear=true)`.
- `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` quando `supports_memory_budget`.
- Registrar no log os limiares do coletor ao iniciar; `DownloadImageMemory` só grava o mip 0.
- Nome do arquivo de binário com o hash da `GraphicsPipelineKey`; remover `LoadLegacyBinary`.
- Pin de subgrupo só nos estágios que o driver lista (`GOW3_SUBGROUP_PIN=allowed` na PR Intel):
  tira o port de fora da especificação na AMD, sem efeito no defeito.

## 7. Como testar (RX 6600)

Cache limpo antes de cada execução (**Clear shader cache**), jogo aberto por um `teste.bat` ao
lado de `God of War III.exe`:

```bat
@echo off
set GOW3_WAVE_STATS=1
set GOW3_GPU_LOG=info
start "" "God of War III.exe"
```

1. Com o arquivo acima: jogar até uma cena com as linhas (a entrada da Gaia serve), fechar e
   anexar `user\last_run.log` **e a pasta `user\shaders\wave_dumps` zipada**, dizendo se as
   linhas apareceram.
2. Trocar as duas linhas `set` por `set GOW3_FULL_BARRIERS=1`: linhas sumiram ou não.
3. Quem tiver o Vulkan SDK: `set GOW3_VK_VALIDATION=1` e `set GOW3_VK_VALIDATION_SYNC=1`, jogar
   até as linhas, fechar e anexar o log.
4. Quem tiver uma RDNA2 **sem** as linhas (6700 XT, 6800, 6900 XT): a execução 1 é o controle
   que falta. Mesmos anexos.

## 8. Perguntas em aberto

1. O que `user\shaders\wave_dumps` mostra para os pixel shaders da Navi 23: wave32 ou wave64?
   E na 6700 XT?
2. `GOW3_FULL_BARRIERS=1` muda alguma coisa?
3. O primeiro evento com blocos 8x4 é draw ou dispatch, e em que wave size?
4. A diferença Navi 23 e Navi 22 é só a Infinity Cache ou o driver escolhe caminhos diferentes
   (DCC, wave size padrão) por família?
