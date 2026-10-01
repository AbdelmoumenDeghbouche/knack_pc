# Temporal upscaling and frame generation — frame analysis and plan

Frame analyzer: `BB_CAPTURE_TRIGGER=<file> BB_CAPTURE_DIR=<dir>`; creating the file records
the next frame (boundary: the pass writing a display buffer) — passes, targets, shaders,
sampled textures, and the first 1 KiB of bound constants for small passes and first draws.

## Bloodborne's frame (1920x1080, Hunter's Dream / Nightmare)

| Passes | What |
|---|---|
| first pass of a frame | copy of the previous UI target into the display buffer (sRGB) |
| shadow | 4096x4096 D32 depth |
| G-buffer | 6 targets 1920x1080 (RGBA8 x3, sRGB albedo, B10G11R11, RGBA16F) + D32S8 depth |
| lighting | light volumes into two B10G11R11 targets |
| scene color | RGBA16F 1920x1080, also forward/transparent draws and effects |
| volumetric fog | compute, reads linear depth (R32F 1920x1080) and composites into scene color |
| half-res effects | RGBA8 960x540 with half-res depth |
| post | combine/bloom pyramid in RGBA16F / B10G11R11 |
| tonemap | into RGBA8 1920x1080 (the UI target) |
| game AA | ping-pong RGBA8 1920x1080 after tonemap (to be skipped when upscaling) |
| UI | stencil-masked draws over the same RGBA8 target |

There is no velocity buffer, also with the camera moving: motion vectors are computed.

## Scene constants (864 bytes, bound by most passes)

Signature: `[0]=3000 (far) [1]=1/3000 [4]=1920 [5]=1080 [6]=1/1920 [7]=1/1080`.

| Floats | Meaning |
|---|---|
| 8–19 | view matrix, 3x4 rows (rotation + translation); the only block that changes with the camera |
| 36–51 | inverse projection (0.700285 = 1/1.42799, 0.39391 = 1/2.53865) |
| 52, 57, 62, 63 | projection (rows 52–67), D3D depth 0..1: x 1.42799, y 2.53865, z 1.00002 / -0.0500679 (near 0.05, far 3000) |
| 112–175 | shadow cascade matrices |
| 180–191 | inverse view (camera to world), 3x4 (176–179: 2, 8, 15, 0) |

The previous frame's matrices are not there; the port keeps them itself.

## Plan

1. Find the scene constants every frame (signature), keep the previous view/projection.
2. Camera motion vectors: compute pass from depth and current/previous matrices into an
   RG16F target; debug view to check them. Object motion later (vertex shader replay with the
   previous frame's constants through the recompiler).
3. Jitter: sub-pixel offset of clip-space positions in the scene passes (recompiler).
4. Upscaler at the scene color stage (before post and UI): FSR 3.1 first (open, native Vulkan,
   also frame generation), then DLSS (native on Linux), XeSS/XeFG and OptiScaler through a
   loader for their Windows DLLs.

## Status

- Camera motion vectors: verified by reprojecting the previous frame (`BB_DEBUG_MOTION=1`,
  toggles 1<<22 reprojected frame, 1<<23 error map): static geometry matches.
- FSR 3.1 (`BB_UPSCALER=fsr3`, FireBurn/FSR-Vulkan submodule): scene color before the
  post-processing combine (compute shader 9a9cf8a9), 1:1, RGB written back (the game keeps data
  in the alpha). Toggle 1<<24 switches it off at run time.
- Jitter: viewport offset of scene geometry (drawn with the scene depth, not full-screen
  quads), Halton(2,3) 8 phases; sign checked by sharpness (correct 255, flipped 208, off 271 —
  1:1 jitter trades high-frequency aliasing for a slightly softer image). Toggle 1<<25 off.
- Frame-to-frame difference while standing still: ~33% lower with FSR.
- Missing: motion of animated objects (characters, cloth, foliage), reactive/transparency
  masks for particles and fog, render-resolution scaling, frame generation.

## 2026-10-01: FSR 4 при выводе 1440p/2160p не работал (мерцание и дрожание)

Видео `video_2026-10-01_03-58-59.mp4` (вывод 3840x2160, FSR 4 Performance): мерцание и дрожание
всех объектов. Причина — апскейлер в этом режиме вообще не выполнялся, а jitter оставался
включённым: на экран шёл растянутый кадр сцены, каждый кадр сдвинутый на свою фазу Halton.

1. Проход UI опознавался по точному размеру цели `BB_RENDER_RES` (1916x1078), а игра выделяет
   цели с выровненной высотой (1916x1080; в константах сцены тоже 1916x1080). `RunScaled` не
   вызывался ни разу (в логе не было `UI: native composition`). Теперь допускается выравнивание
   до 8 пикселей (`RenderTarget`), а размер сцены для FSR берётся из констант сцены
   (`CameraMotion::RenderSize`, `SceneSize`).
2. После этого FSR 4 падал с `external image registration failed (-1000069000)`: `RunScaled`
   создавал новые image view каждый кадр, а реестр FSR 4 вмещает восемь. Теперь те же
   `CachedView`, что и в пути Native AA.

Проверка дампом (`BB_DUMP_TRIGGER=<файл> BB_DUMP_DIR=<каталог>`, `BB_DUMP_FRAMES`, по умолчанию 8:
вход FSR, векторы движения и выход, raw): при неподвижной камере PSNR соседних кадров выхода
~45 дБ против ~28 дБ у входа с jitter; при повороте камеры и ходьбе шлейфов нет. FPS в этом
режиме ~107 вместо ~220 — раньше FSR 4 просто не выполнялся.

Осталось: спрайты (по 4 индекса) в цвет сцены со сценической глубиной — свечения, огоньки —
по-прежнему не сдвигаются jitter (правило «≤ 6 индексов = полноэкранный проход»).

## 2026-10-01: FSR 4 быстрее — проход post (2.9 → 0.8 мс в 4K)

Замеры: `BB_FSR4_PROFILE=1` (время каждого прохода FSR 4, патч провайдера в субмодуле,
см. `gpu/patches/fsr-vulkan`), бенчмарк вне игры `out/gpu/fsr4-bench` (`ninja -C out/gpu
fsr4-bench`; `--stats` — регистры и инструкции от RADV). 4K Balanced (2260x1272 → 3840x2160),
RX 7800 XT: весь FSR 4 — 5.8 мс, из них **post 2.5–2.9 мс** (не нейросеть: последние слои,
pixel shuffle 2x2 и смешивание с историей), 12 проходов модели — 2.4 мс, pre 0.55.

Причина: каждый поток считает блок 2x2 выходных пикселей и пишет их по одному в три образа
(рекуррентное состояние, история, выход) — каждая инструкция записи волны пишет пиксели через
один. Убрав любую из трёх записей, проход ускорялся в 1.3–4 раза при том же коде.

Решение: `tools/fsr4_optimize.sh` декомпилирует post (spirv-cross), `tools/fsr4_post_lds.pl`
собирает блок 16x16 рабочей группы в shared memory и пишет сплошными строками, glslang
компилирует обратно в `fsr4_shaders/opt/`; `vk_fsr4.cpp` берёт его оттуда (`BB_FSR4_OPT=0` —
оригинал). Две тонкости, найденные сравнением выходов:
- spirv-cross переводит знаковую распаковку int8 (`OpBitcast` в `i8vec4`) как `unpack8(uint)`
  (беззнаковую) — без исправления результат совсем другой (PSNR 17 дБ);
- значения в shared memory должны оставаться float: half из shared memory компилятор
  превращает в 16-битную запись, а она иначе округляет в unorm8.

`tools/fsr4_verify.sh` сравнивает оригинал и оптимизированный post для всех пресетов при выводе
1080p/1440p/4K на псевдослучайных входах: 1440p и 4K — побитово одинаково; post 4K 2.1–3.5 →
0.81–0.87 мс, 1440p 1.2–1.5 → 0.36–0.38 мс, 1080p 0.37–0.69 → 0.20–0.22 мс. В игре (4K
Balanced): FSR 4 5.8 → 4.0 мс, кадр GPU 13.1 → ~11.6 мс; дальше FPS упирается в CPU (поток
GPU-команд ждёт копии гостевой памяти, «host copies» ~20%).

Найдено попутно: **оригинальный FSR 4 на уровне 1080 (вывод 1920x1080) недетерминирован** — от
запуска к запуску на одинаковых входах меняется полоса у левого края (столбцы 0–132), то есть
где-то гонка или чтение неинициализированной памяти в модели/провайдере. При выводе 1080p это
может давать мерцание у левого края кадра. Не исследовано.

Маска реактивности при FSR 4 больше не считается (FSR 4 её не принимает).

### Что ещё проверено (там же, 4K Balanced)

- **A/B в одном процессе** (`ab.sh`, бит 24 — FSR выключен, UI-копия без апскейла): 86.5 FPS с
  FSR 4 против 87.7 без него. После оптимизации post кадр упирается в CPU (поток GPU-команд,
  ожидание копий гостевой памяти), а не в GPU. Async compute для FSR (перекрыть его с началом
  следующего кадра) дал бы не больше этого ~1% — отложено до ускорения CPU-части; к тому же он
  требует переставлять команды UI и вывода кадра N после работы кадра N+1.
- **WMMA (`VK_KHR_cooperative_matrix`, RADV на RDNA3 поддерживает)**: прототип прохода 1
  модели (остаточный блок 16 каналов: 3x3 16→16, 1x1 16→32 ReLU, 1x1 32→16) — 0.85 мс в первом
  варианте, 0.49 мс со словной раскладкой в shared memory, против 0.30 мс у исходного dot4.
  Арифметика на WMMA заняла бы ~0.1 мс, но при 16 каналах выкладка тайлов, эпилоги через shared
  memory и занятость (6 волн/SIMD, предел по LDS) съедают выигрыш. Потолок — около 1 мс на
  все 12 проходов при тонкой ручной переработке каждого; не начато.
- `RADV_PERFTEST=cswave32` (wave32 для compute): FSR 4 медленнее, 4.2 → 5.4 мс.
- Проход pre (0.54 мс) запись не ограничивает (0.50 мс без записи).

## 2026-10-01: мерцание FSR 4 у левого края при выводе 1080p — гонка в проходе 11 модели

Недетерминизм уровня 1080 (см. выше) — ошибка в шейдере модели v07, проход 11 (декодер,
1/4 → 1/2 разрешения). Каждый поток — пиксель входа 1/4 разрешения и пишет блок 2x2 выхода
1/2 разрешения. Диспатч округляет ширину до 64 потоков, а потоки за шириной входа не
останавливаются: на уровне 1080 (вход 480 в ширину) потоки 480..511 пишут пиксели выхода
960..1023, то есть первые пиксели следующей строки (строка любого тензора — 15360 байт), и
гоняются с их настоящими авторами. При выводе 4K ширина 960 делится на 64, ошибки нет.

`tools/fsr4_pass11_guard.pl` добавляет ранний выход для потоков за границей (размер берётся из
проверки соседей самого прохода); `tools/fsr4_optimize.sh` собирает его в `fsr4_shaders/opt`
для всех пресетов вместе с post; `vk_fsr4.cpp` и бенчмарк берут из `opt/` любой проход.
`tools/fsr4_verify.sh`: 1440p/4K — побитово как оригинал (защита там не срабатывает); 1080p —
одинаковый результат от запуска к запуску, отличия от оригинала после первого кадра только в
бывшей полосе у левого края.

Найдено при этом: spirv-cross переводит все знаковые распаковки int8 (`OpBitcast` в `v4char`)
как беззнаковый `unpack8(uint)`; в pass 11 их девять разных форм. Исправление общее для
скриптов — `tools/Fsr4SpirvCrossFixes.pm` (все `unpack8` через знаковый помощник).
