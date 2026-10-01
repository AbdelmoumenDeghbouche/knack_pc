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
