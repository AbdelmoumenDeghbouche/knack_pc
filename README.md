# Knack for PC (knack_pc)

A native PC port of **Knack** (PlayStation 4, 2013) for Windows 10/11, in the style of
[bbport](https://github.com/deadinside28/bloodborne_pc): the game's original x86-64 code runs
directly on the PC's processor, a runtime replaces the PS4 system libraries, and the graphics are
translated to Vulkan by the shadPS4 video core. There is no CPU emulator.

This repository started as a copy of [wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc)
(the WipEout Omega Collection port). Everything below the WipEout layer is shared; this README
lists what was added for Knack.

> **Status: early development. The game boots, saves, plays its story videos and the first level
> (the training rooms) at up to 120 FPS, with music and sound effects.** Later levels are not
> tested yet. See [Progress](#progress) and [Known issues](#known-issues).

> **No game files are included.** You need your own copy of the game, dumped from your own PS4.
> This project is not affiliated with Sony Interactive Entertainment or Japan Studio. Knack is a
> trademark of Sony Interactive Entertainment.

## Progress

| Step | State |
|---|---|
| Unpack the game and its update | Done (`scripts/pkg_extract.py`) |
| Load and link the executable (eboot.bin, libc, Fios2) | Done |
| PS4 system functions Knack 1.10 imports | Done: the 39 that were missing (`python run.py --check-imports` reports 0) |
| Memory map | Done: Knack maps its direct memory at the fixed address 0x7f0000000 |
| Boot, menus, saves, story videos | Done |
| Sound: music and effects (NGS2) | Done for the first level |
| First level (training rooms) | Plays: fights, doors, the next rooms |
| Game speed | Done: the game steps by elapsed time, so it runs at normal speed at any frame rate |
| Frame rate above 60 FPS | Done: vblank 120 Hz, 71–120 FPS in the first fight on an RTX 4070 Laptop |
| Long play without slowdown or memory growth | Done: flat 60 FPS and about 4.4 GB over long runs (was 55 → 4 FPS) |
| Later levels | Not tested yet |
| EU versions (CUSA00006, CUSA00202) | Not tested yet |
| Windows package and launcher for Knack | Not yet: start it with `run.py` |

## Supported game versions

| Title ID | Region | Version | State |
|---|---|---|---|
| CUSA00068 | US | 01.00 + update 01.10 | Development target |
| CUSA00006 | EU | — | Untested (same game profile) |
| CUSA00202 | EU | — | Untested (same game profile) |

## What was built for Knack

### System functions (`src/runtime_*.c`, `src/import_names.inc`)

The 39 functions Knack imports that the WipEout runtime did not have:

- **Json2** (`runtime_json.c`): value copy, `set` for bool, integer, unsigned, real, string and
  value type, object begin and empty checks, object and array iterators.
- **Network, PSN** (`runtime_services.c`): NetCtl init, HTTP request creation, NpAuth timeouts
  and async waits, NpScore async calls and polling, NpTus title contexts and multi-slot and
  multi-user variables. They answer as an offline console.
- **PlayGo**: the language mask (bit 63 minus the language number).
- **Rtc** (`runtime_rtc.c`): RFC 3339 local time, add ticks, add seconds.
- **Audio** (`runtime_audio.c`): the pad speaker mix level.
- **Save data** (`runtime_savedata.c`): save parameters (types 0–5).
- **PngDec** (`runtime_extra.c`): PNG header parse, create, delete, decode (through the GPU
  library's PNG decoder), alpha fill and the BGRA order.
- **Kernel**: the current CPU number, the 1 GB temporary space.

### Memory map (`src/runtime_memory.c`)

Knack maps its direct memory at the fixed address 0x7f0000000, inside the PS4's system-managed
range. On Windows the loader now also reserves 0x400000000–0x800000000 at start, so that address
is free when the game asks for it.

### Red zones (`src/redzone.cpp`)

Some game functions keep values in the 128 bytes below the stack pointer (the SysV red zone).
A GPU write fault on Windows can overwrite them. Knack crashed this way after trophy 22: a
function at 0x55c410 lost a pointer it kept there and wrote to address 0. The Knack profile
turns the protection on (`GOW3_RED_ZONE=1`). The patcher now also reads a jump table whose
bounds check is on the register the index is copied from (`mov ebp, eax; cmp eax, 3; ja`). Its
liveness analysis follows the jump table targets. Before, 18 accesses were left unprotected,
and after the first change the analysis saw the function's red zone as unused. Now all 1488
faultable accesses are rerouted.

### Sound: NGS2 (`src/ngs2/`)

Knack's sound engine (Sony's Sndx) plays everything through libSceNgs2.

- **The NGS2 implementation** comes from [AnyPS5](https://github.com/boykopovar/AnyPS5)
  (`core/libs/prx/libSceNgs2.native`, GPL-2.0): systems, racks, voices, sampler, submixer,
  mastering, reverb, panning, ATRAC9.
- **PS4 → PS5 translation** (`Ngs2Exports.cpp`): AnyPS5 uses the PS5 layouts. The wrapper
  converts the PS4 ones: the 64-byte system option, the 128-byte rack options, the 32-bit
  waveform blocks, voice events (number n → bit n), the 4, 16 and 48-byte voice states and the
  48-byte sampler filter.
- **Errors stop cleanly** (`ngs2_compat.hpp`): AnyPS5 throws C++ exceptions, which cannot unwind
  through the game's own frames. They print `STOP: <reason>` and exit instead.
- **HE-VAG decoding** (`Voice.cpp`, `hevag_coefs.h`): Knack's sound effects are HE-VAG (High
  Efficiency VAG), not classic VAG: 128 four-tap predictors instead of 5. Decoding them as classic
  VAG gave the "old TV" noise. The decoder and its coefficient table follow
  [vgmstream](https://github.com/vgmstream/vgmstream) (ISC).
- **Sampler filters**: all ten PS Vita NGS filter modes (low-pass, high-pass, band-pass, notch,
  peaking, shelves, one-pole), as RBJ biquads.
- **Effects NGS2 did not have**, added from the PS4 parameter layouts:
  - sampler envelope (0x10000006): fade-in points after play, release points after stop;
  - submixer compressor (0x20000002): threshold, ratio, attack, release, output level;
  - submixer equalizer (0x20000006): the sampler filter run on the submixer's mix.
- **Output channels**: mastering voices mix into 1, 2, 6 or 8 channels (the pad speaker has 1).
- `GOW3_NGS2_SKIP_UNKNOWN=1` (on in the Knack profile) skips and logs any other parameter a later
  level may send, instead of stopping.

### Graphics

- **Pipeline cache key** (`vk_pipeline_cache.cpp`): the shader permutation key was taken after the
  clip-distance pass had changed the runtime info, so a stored permutation never matched. Every
  frame compiled new shaders: FPS fell from 55 to 4 over minutes and memory grew until Windows ran
  out. The key is now taken before translation.
- **Draw preparation off** (game profile, mask 4096 | 8192): the draw-preparation workers read
  Knack's resource descriptors before the game had written them, and gameplay geometry exploded.
  Found by switching optimizations live (`GOW3_TOGGLE_FILE`).
- **Shader translator** (`vector_alu.cpp`), shader cache version 9:
  - `V_MAC_LEGACY_F32` and `V_MAD_LEGACY_F32` use the DX9 multiply (0 × anything = 0);
  - `V_CVT_PKRTZ_F16_F32` rounds toward zero like the hardware: a value past the half-float range
    becomes ±65504, never infinity;
  - `V_RCP_LEGACY_F32` takes the sign of an infinite result from its bits.
- **Blending on swapped color targets** (`AlphaToSrc1` in `vk_pipeline_cache.cpp`): fixed the
  black and white "TV static" in smoke, dust, explosions and super moves. Knack's HDR target
  stores its channels reversed (A, B, G, R). GCN blends before that swap, so `SrcAlpha` is the
  shader's alpha; Vulkan blends the stored order and took red as alpha. Bright smoke made
  1 − "alpha" negative. The smoke also samples a half-size copy of that target (heat haze), so the
  error grew each frame up to ±65504 (found by dumping the copy: `GOW3_DIAG_ADDR`). Now such
  draws also write their alpha as the dual-source second output, and the blend reads it
  (`Src1Alpha`). Test: 0 noisy screenshots in 3 × 126 (before: 2–6 per run), game clock at half
  speed (`GOW3_TSC_SCALE=2`), where the static came most often.

### Frame rate

Knack steps its game logic by the time between frames, so it runs at normal speed above 60 FPS.
The Knack profile sets the vblank to 120 Hz (`GOW3_VBLANK_HZ=120`): up to 120 FPS.

## Known issues

- **Separate alpha blending on swapped targets**: the stored 4th channel is a color, so it now
  uses the color equation; a draw with a different alpha equation gets that on its alpha channel
  too. None seen in Knack so far.
- **Menu text** can glitch for the first frames after boot.
- **Later levels** may send NGS2 parameters that are not implemented: they are skipped and logged
  as `NGS2: voice param ... skipped`.

## Building (Windows)

1. Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`) and, in the
   **MSYS2 CLANG64** shell, the packages listed in
   [packaging/windows/README.md](packaging/windows/README.md).
2. Install Python 3.10+ from python.org (with Tk), then:

   ```powershell
   python -m pip install pillow iced-x86==1.21.0 cryptography
   ```

3. Build in the MSYS2 CLANG64 shell, from this folder (with its submodules):

   ```bash
   bash build.sh
   ```

## Preparing your game

From fake PKGs of your own dump (base game and update):

```powershell
python scripts/pkg_extract.py <base.pkg> D:\games\Knack\CUSA00068
python scripts/pkg_extract.py <update.pkg> D:\games\Knack\CUSA00068-patch
```

## Running

```powershell
$env:GOW3_GAME_DIR = 'D:\games\Knack\CUSA00068'
$env:GOW3_PATCH_DIR = 'D:\games\Knack\CUSA00068-patch'
python run.py
```

The game profile (`scripts/game_profiles.py`) sets the Knack options by title ID. The first start
after an update compiles the shaders again: expect some stutter.

List the PS4 functions the runtime does not have yet:

```powershell
$env:GOW3_MODS_ENABLED = '0'
python run.py --check-imports
```

## Testing and debugging tools

Added while porting Knack. All are environment variables for `run.py`.

| Variable | What it does |
|---|---|
| `GOW3_BACKGROUND_WINDOW=1` | Opens the window off-screen, not activated and not focusable: test runs do not steal the focus |
| `GOW3_AUDIO=none` | Silent run (timer audio backend) |
| `GOW3_PAD_FILE=<file>` | Scripted controller input: button names, `lx= ly= rx= ry=`, `shot` for a screenshot |
| `GOW3_FRAME_STATS=1` | FPS, worst frame and GPU thread load every few seconds |
| `GOW3_TOGGLE_FILE=<file>` | Switches GPU optimizations while the game runs (write a bit mask to the file) |
| `GOW3_DISABLED_OPTIMIZATIONS=<mask>` | The optimizations switched off at start |
| `GOW3_TSC_SCALE=<n>` | The game's clock runs 1/n: a 60 FPS run steps the game like n × 60 FPS (finds high frame rate bugs on a slow or busy GPU) |
| `GOW3_SKIP_PS=<ps>[/<vs>[/<index count>]][,...]` | Drops the draws of a pixel shader, optionally only with that vertex shader and index count |
| `GOW3_CAPTURE_TRIGGER=<file>`, `GOW3_CAPTURE_DIR=<dir>` | Creating the trigger file records the next frame: passes, shaders, textures, constants |
| `GOW3_CAPTURE_PS=<hash>` | The recording keeps every draw of that pixel shader, with its constants, vertex streams, attribute formats and the image each texture slot resolved to |
| `GOW3_DIAG_IMAGES=1`, `GOW3_DIAG_DIR=<dir>` | With `GOW3_CAPTURE_PS`: a recording also writes the raw textures of that shader's first draws |
| `GOW3_DIAG_ADDR=<address>` | Only the image at that guest address, first draw only (the full dumps can fill a disk in minutes) |
| `GOW3_CAPTURE_FULL=<address>` | That buffer is dumped whole, once per recording |
| `GOW3_CAPTURE_DOWNLOAD=<address>:<bytes>:<cs hash>` | The GPU's own copy of a region after compute dispatches (data the CPU never sees) |
| `GOW3_PATCH_SHADERS=1` | `user/shader/patch/<stage>_<hash>_<perm>.spv` replaces that shader when it is compiled |
| `GOW3_PERM_LOG=1` | Logs shader permutations that differ from the closest stored one |
| `GOW3_NGS2_LOG_SKIPPED=1` | Prints the words of skipped NGS2 parameters (and envelope points) |
| `GOW3_NGS2_DUMP_VAG=<dir>` | Saves the first VAG waveforms, raw and decoded |
| `GOW3_AUDIO_DUMP=<prefix>` | Records every audio port to a file |

## How it works

| Part | Where | What it does |
|---|---|---|
| PKG unpack | `scripts/pkg_extract.py` | Decrypts and unpacks fake PKGs (PFS, PFSC), as shadPS4 does |
| Game profiles | `scripts/game_profiles.py` | System modules and settings per title ID |
| Preparation | `scripts/prepare.py`, `link_libc.py`, `link_modules.py` | Reads `eboot.bin` and links the game's own `libc` and `libSceFios2` |
| Import names | `scripts/gen_import_names.py`, `src/import_names.inc` | Maps the game's import NIDs to function names |
| Loader | `src/probe.c` | Loads the image and starts the game's original code |
| Runtime | `src/runtime_*.c` | PS4 system functions |
| NGS2 | `src/ngs2/` | Sound voices, racks and effects (AnyPS5 + PS4 translation) |
| GPU | `gpu/shadps4/` | shadPS4 video core (Vulkan) |

Many names (`gow3-probe.exe`, `GOW3_*` variables, the launcher) still come from the God of War III
port this project is based on.

## Credits

- **wipeout_pc**: [AbdelmoumenDeghbouche/wipeout_pc](https://github.com/AbdelmoumenDeghbouche/wipeout_pc),
  the base of this port, and everything it credits:
  [FelipePrado0's gow3_pc](https://github.com/FelipePrado0/gow3_pc),
  [bc160201983's MediEvil port](https://github.com/bc160201983/medievil_2019_pc_port),
  [deadinside28's bloodborne_pc](https://github.com/deadinside28/bloodborne_pc),
  [Supermedo's Windows port](https://github.com/Supermedo/bloodborne_pc),
  [cuesta4's shadPS4 fork](https://github.com/cuesta4/shadPS4/tree/eltutz) and the
  [shadPS4](https://github.com/shadps4-emu/shadPS4) team.
- **AnyPS5**: [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5), the NGS2 implementation
  (GPL-2.0).
- **vgmstream**: [vgmstream](https://github.com/vgmstream/vgmstream), the HE-VAG decoder and
  coefficients (ISC).
- **Robert Bristow-Johnson**: the Audio EQ Cookbook filter formulas.

Also used: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK,
[MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) and [Pillow](https://python-pillow.org).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses
(vgmstream's table in `src/ngs2/hevag_coefs.h`: ISC).
