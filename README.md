# bbport — a native Linux port of Bloodborne

**English** · [Русский](README.ru.md)

bbport runs the original PlayStation 4 executable of *Bloodborne* (CUSA03173, game version
1.09) directly on an x86-64 Linux PC. It is not a general emulator. The game's own x86-64 code
executes natively; a small runtime written for this one game replaces the PS4 system libraries;
the GPU work is translated to Vulkan by a renderer derived from
[shadPS4](https://github.com/shadps4-emu/shadPS4) and heavily extended for this game, including
temporal upscaling with AMD FSR 3.1, FSR 4 and FSR 4.1.1.

> **No game files are included.** You need your own dump of Bloodborne (CUSA03173, v1.09).
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware or AMD.

**Status: experimental, playable.** The game boots, loads saves and plays (the Hunter's Dream
and several areas of Yharnam were played with it) with sound, gamepad and saving.
A full play-through has not been verified, and only one machine (Linux, AMD Radeon RX 7800 XT,
Mesa/RADV) has been tested thoroughly.

## Highlights

- **Native execution.** The eboot is converted offline into a flat memory image; PS4 libc and
  libSceFios2 are linked into it as native code. No CPU emulation and no per-instruction
  translation: the game code runs at full speed.
- **Unlocked frame rate.** Community patches (`patches/Bloodborne.xml`) make the simulation
  use the real frame time; ~90 FPS at 4K with FSR 4 Balanced on an RX 7800 XT, ~150 FPS at
  1440p with FSR 4 Quality. Also 30/60/90 FPS modes.
- **Temporal upscaling built for this game.** Bloodborne has no velocity buffer, so bbport
  computes motion vectors itself: camera motion from depth and the scene matrices, and object
  motion (characters, cloth, weapons) from the vertex positions of the previous frame. The
  scene is jittered sub-pixel (Halton) and rendered at a reduced resolution; the upscaler fills
  the output (720p for the Steam Deck, 1080p, 1440p or 2160p) and the UI is drawn natively at the output resolution.
  - **FSR 3.1** (FireBurn/FSR-Vulkan).
  - **FSR 4 (INT8, model v07)** on any GPU with integer dot products — RDNA2/3 included.
  - **FSR 4.1.1 (INT8)**: AMD's 4.1.1 DLL is recorded once under vkd3d-proton and its passes
    are replayed natively on Vulkan; the output is **bit-exact** with the DLL. The assets are
    built on your machine from your own DLLs (`tools/fsr4cap`).
  - Faster than AMD's own shaders on RDNA3: the final passes of FSR 4 and 4.1.1 were rewritten
    to store through workgroup memory (3.5× and 2.3× faster, bit-exact); FSR 4 costs ~4 ms at
    4K on an RX 7800 XT instead of ~6 ms.
- **Multi-threaded GPU command processing.** The PS4 command stream is decoded on one thread
  and draws are bound and recorded on another (two-stage pipeline), with a Vulkan recording
  thread and helper threads for memory copies. Early on the single GPU thread capped the game
  at ~26 FPS; now it runs at 90–150 FPS depending on resolution and scene.
- **In-game menu** (Insert or L3+R3): upscaler, preset, sharpness, output resolution, game
  effects (chromatic aberration, DoF, motion blur, SSAO, the game's own AA, SSR, model LOD).
- **GTK4 launcher** and an **AppImage** for the Steam Deck.

## How it differs from shadPS4

| | shadPS4 | bbport |
|---|---|---|
| Scope | General PS4 emulator, many games | One game: Bloodborne v1.09 |
| Loading | Its own ELF loader and kernel emulation at run time | The eboot is converted offline (`scripts/`) into an image with PS4 libc/Fios2 linked in; a C loader maps it and jumps into the game (loader and runtime: ~5k lines) |
| System libraries | Broad HLE of the PS4 OS | A small runtime (`src/runtime_*.c`) that implements exactly what Bloodborne calls: memory, threads, sync, files, audio (incl. ATRAC9), pad, saves, AppContent |
| GPU | shadPS4 video core and shader recompiler | The same core (vendored, GPL) with ~200 marked changes (`bbport:`) plus new modules: two-stage draw pipeline, render-state and texture-set memoization, render-scale proxies, motion vectors, FSR 3.1/4/4.1.1, frame capture and GPU profiler |
| GPU thread | One thread processes the whole command stream (the bottleneck in Bloodborne) | Decode and draw recording run on separate threads; the work scales with the hardware threads (Steam Deck included) |
| Upscaling | — | Temporal (FSR 3.1, FSR 4, FSR 4.1.1) with the game's own motion vectors and jitter |
| Game patches | Patch files applied by the emulator | The same community patches, compiled at start (`scripts/patches.py`); render resolution, effects and FPS from the launcher |

Without shadPS4 there would be no bbport: its renderer and shader recompiler are the base of
the graphics side.

## Requirements

- Linux x86-64, a Vulkan 1.3 GPU. Tested: AMD RX 7800 XT with Mesa 26 (RADV). FSR 4 / 4.1.1
  need integer dot products (RDNA2 and newer; NVIDIA and Intel should work but are untested);
  FSR 4.1.1 also needs `VK_VALVE_shader_mixed_float_dot_product` (RADV).
- Your decrypted game dump: the `CUSA03173` folder (eboot.bin, sce_module, ...), version 1.09.
- To build: GCC, CMake, Ninja, Python 3, glslang, SDL3, Vulkan headers and the libraries in
  `shell.nix`. With [Nix](https://nixos.org) everything comes from `shell.nix` automatically.

## Build and run

```bash
git clone --recursive <this repository> bbport && cd bbport
bash build.sh                        # builds out/bb-probe and out/gpu/libbbgpu.so
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

or the launcher (pick the game folder, settings, *Start*):

```bash
bash launcher/bb-launcher.sh         # launcher/install-desktop.sh adds it to the app menu
```

By default the game folder is expected next to the repository (`../CUSA03173`). Saves and the
shader cache go to `user/` (the launcher lets you choose another folder); settings to
`bbport.ini`. A gamepad is used through SDL3; there is a keyboard fallback.

**Upscaler assets** (not included; FSR 3.1 needs none):

```bash
bash tools/fetch_fsr4_assets.sh      # FSR 4 v07 (MIT, built from AMD's source by Q2RTX)
# FSR 4.1.1, from your own AMD DLLs (e.g. OptiScaler's FSR4_LATEST), needs Proton (GE-Proton):
bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll> <amd_fidelityfx_loader_dx12.dll>
```

**AppImage** (Steam Deck): `bash build.sh && bash packaging/appimage.sh` →
`dist/Bloodborne-bbport-x86_64.AppImage`; data in `~/.local/share/bbport`, `--play` starts the
game without the launcher window (Game Mode). On the Steam Deck pick the 1280×720 output (the
game is 16:9; on the 1280×800 screen it gets thin bars).

**Adding the AppImage to Steam** (*Add a Non-Steam Game*): leave *Compatibility* off. Where Steam
runs games without FUSE (NixOS: Steam's FHS sandbox; the AppImage then exits with *Cannot mount
AppImage*), set the launch options to

```
TMPDIR=$HOME/.cache APPIMAGE_EXTRACT_AND_RUN=1 NO_CLEANUP=1 %command%
```

The AppImage then unpacks itself (~2 GB, `~/.cache/appimage_extracted_*`) on the first start
(~10 s) and reuses that copy afterwards; a new AppImage version gets a new copy, the old one can
be deleted. Without `TMPDIR` it would unpack into Steam's `/tmp`, which is in RAM there. Add
` --play` after `%command%` to skip the launcher.

Useful variables: `BB_FRAME_STATS=1` (frame statistics), `BB_GPU_PROFILE=1` (GPU time per
pass), `BB_FSR4_PROFILE=1` (GPU time per FSR 4 pass), `BB_UPSCALER=fsr3|fsr4|fsr411|none`.
More in [docs/](docs).

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and the HLE runtime |
| `scripts/` | Offline preparation of the game image, module linking, patch compiler |
| `gpu/` | Renderer library: vendored shadPS4 video core with this port's changes (`gpu/VENDOR.txt`), shims, ImGui menu, FSR 4.1.1 runtime (`gpu/shadps4/video_core/renderer_vulkan/fsr411`) |
| `launcher/`, `packaging/` | GTK4 launcher; Nix package and AppImage |
| `patches/` | Community patches for Bloodborne |
| `tools/` | Developer tools: scripted runs, A/B toggles, FSR benchmark helpers, FSR 4 shader rewrites, `fsr4cap` (FSR 4.1.1 recording/extraction) |
| `tests/` | Loader, runtime, patch and renderer tests |
| `docs/` | Design notes and measurements ([upscaler](docs/upscaler.md), [parallel GPU](docs/parallel_gpu.md), [motion vectors](docs/motion_vectors.md), [roadmap](docs/ROADMAP.md)) |

Tests: `bash build.sh --test`, `python3 -m unittest discover -s tests`, and
`ninja -C out/gpu motion-history-test ui-composition-test scene-resolution-test motion-shader-test`.

## Roadmap

- More CPU parallelism in GPU command processing (split the draw-recording stage further),
  scaling to all hardware threads — most important for the Steam Deck.
- Async compute for the upscaler (the frame is GPU-bound at 4K).
- XeSS (super resolution) and XeFG frame generation through a Wine helper sharing Vulkan
  memory (a memory-bridge prototype is in `tools/bridge_helper`); DLSS for NVIDIA users;
  inputs exposed so that OptiScaler-style mapping works.
- Frame generation (FSR 3.1 FG first), reactive and transparency masks for particles and fog.
- Fix the races in AMD's FSR 4.1.1 shaders at output widths that are not multiples of 64
  (e.g. 1600×900), as already done for the left-edge race in FSR 4 v07 at 1080p.
- Steam Deck validation of the AppImage; HDR output.

## Credits and licenses

bbport is licensed under the **GNU GPL v2 or later** ([LICENSE](LICENSE)) — it contains code
from shadPS4 (GPL-2.0-or-later). Third-party components keep their licenses:
[shadPS4](https://github.com/shadps4-emu/shadPS4) video core and shader recompiler (GPL-2.0+),
[sirit](https://github.com/shadps4-emu/sirit), [half](https://half.sourceforge.net/),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) by FireBurn (MIT; FSR 3.1 on Vulkan and the
FSR 4 v07 provider), AMD FidelityFX SDK (MIT), [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9)
(MIT), [Dear ImGui](https://github.com/ocornut/imgui) (MIT), DejaVu fonts,
[dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT, used to build the
FSR 4.1.1 assets). Game patches by Kyo, Lance McDonald, auser1337, illusion, emoose and other
community members (`patches/Bloodborne.xml`). AMD's FSR 4 DLLs and model data are not
distributed here.
