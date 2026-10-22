# WipEout Omega Collection for PC (wipeout_pc)

A native PC port of **WipEout Omega Collection** (PlayStation 4) for Windows 10/11, in the
style of [bbport](https://github.com/deadinside28/bloodborne_pc): the game's original x86-64
code runs directly on the PC's processor, a runtime replaces the PS4 system libraries, and the
graphics are translated to Vulkan by the shadPS4 video core. There is no CPU emulator.

> **Status: early development. Menus and races (HD and 2048) play above 60 FPS.**
> See [Progress](#progress).

## Download and play (Windows)

1. Download `wipeout-windows.zip` from the
   [latest release](https://github.com/AbdelmoumenDeghbouche/wipeout_pc/releases/latest)
   and extract it.
2. Open `WipEout.exe`, choose your game folder (the one with `eboot.bin`) and click **PLAY**.

Requirements: Windows 10 or 11 (64-bit), a CPU with AVX, a GPU with Vulkan 1.3. The
`README-Windows.txt` in the zip has the details. Linux: build from source (below).

> **No game files are included.** You need your own copy of the game, dumped from your own PS4.
> This project is not affiliated with Sony Interactive Entertainment, XDev, Clever Beans or EPOS
> Game Studios. WipEout is a trademark of Sony Interactive Entertainment.

## Progress

| Step | State |
|---|---|
| Unpack the game and its update | Done (`scripts/pkg_extract.py`) |
| Load and link the executable (eboot.bin, libc, Fios2, NpToolkit2) | Done |
| PS4 system functions the game uses | Done for menus and races; PSN, VR and camera answer as an offline console |
| Boot, menus, saves | Done |
| Races (HD and 2048), race results | Done |
| Game speed | Done: the game runs on real time at any frame rate |
| Frame rate above 60 FPS | Done: up to 240 FPS, set by the frame rate limit (110–150 FPS in 2048 races on an RTX 4070 Laptop) |
| Graphics: Metro Park terrain | Fixed (shader recompiler, #5) |
| Render resolutions: 2560x1080, 5120x2160, native 4K without checkerboard | Done (`patches/WipEout_Omega_Collection.xml`, #2). HUD stays 16:9 |
| HDR | Done; the darker picture when RCAS runs is fixed, waiting for a test (#4) |
| Sound: music and effects (Audio3d) | Done; the effects are new, please report anything that sounds wrong |
| Launcher and settings | Done (`launcher/gow3_launcher_win.py`) |
| Windows package (zip) | Done (`packaging/windows/package.sh`, Actions > Windows zip) |
| Linux | Builds from source; no package yet |
| PS4 Pro mode | Not yet: the shadPS4 texture cache stops on an image overlap |
| PSVR mode | Not planned for now (#7) |
| Every game and event type (HD, HD Fury, 2048) | Not all tested yet |

2048 races need red-zone protection on Windows (`src/redzone.cpp`): the game's profile turns
it on. Without it they run at less than 1 FPS.

The game times itself with the processor's time stamp counter (`rdtsc`), so the runtime gives
it the real counter and its measured frequency (`src/runtime_kernel.c`). Its speed then does
not depend on the frame rate.

## Supported game versions

| Title ID | Region | Version | State |
|---|---|---|---|
| CUSA07671 | US | 01.00 + update 01.07 | Development target |
| CUSA05670 | EU | — | Untested |

The base game and the update are merged into one folder (the update's files replace the base
game's). One `eboot.bin` runs all three games: WipEout 2048, WipEout HD and WipEout Fury.

## Building (Windows)

1. Install [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`) and, in the
   **MSYS2 CLANG64** shell, the packages listed in
   [packaging/windows/README.md](packaging/windows/README.md).
2. Install Python 3.10+ from python.org (with Tk), then:

   ```powershell
   python -m pip install pillow iced-x86==1.21.0 cryptography
   ```

3. Clone with submodules and build in the MSYS2 CLANG64 shell:

   ```bash
   git clone --recursive https://github.com/AbdelmoumenDeghbouche/wipeout_pc
   cd wipeout_pc
   bash build.sh
   ```

   If Git converts line endings on your PC (`core.autocrlf=true`), the FSR patches do not
   apply. Turn it off for this repository and its submodules before building:

   ```bash
   git config core.autocrlf false
   git submodule foreach --recursive git config core.autocrlf false
   git rm -rq --cached . && git reset -q --hard
   git submodule foreach --recursive 'git rm -rq --cached . && git reset -q --hard'
   ```

## Preparing your game

From fake PKGs of your own dump (base game and update):

```powershell
python scripts/pkg_extract.py <base.pkg> D:\games\CUSA07671
python scripts/pkg_extract.py <update.pkg> D:\games\CUSA07671-patch
```

Then move the update's files over the base game folder, or pass the update folder with
`GOW3_PATCH_DIR`. A folder dumped with GoldHEN works the same way.

## Running

Start the launcher, choose your game folder on the **Game** page, then click **Play**:

```powershell
python launcher\gow3_launcher_win.py
```

The **Advanced** page can put a shortcut on the desktop. Settings: display mode, frame rate
limit, VSync, anisotropic filtering, HDR, controls, mods and the game language.

Without the launcher, `run.py` starts the game:

```powershell
$env:GOW3_GAME_DIR = 'D:\games\CUSA07671'
python run.py
```

List the PS4 functions the runtime does not have yet:

```powershell
$env:GOW3_GAME_DIR = 'D:\games\CUSA07671'
$env:GOW3_MODS_ENABLED = '0'
python run.py --check-imports
```

Without `--check-imports`, `run.py` starts the game. The loader stops at the first system
function the runtime does not implement and prints its name.

## How it works

| Part | Where | What it does |
|---|---|---|
| PKG unpack | `scripts/pkg_extract.py` | Decrypts and unpacks fake PKGs (PFS, PFSC), as shadPS4 does |
| Game profiles | `scripts/game_profiles.py` | System modules to link per title ID |
| Preparation | `scripts/prepare.py`, `link_libc.py`, `link_modules.py` | Reads `eboot.bin` and links the game's own `libc`, `libSceFios2` and `libSceNpToolkit2` |
| Import names | `scripts/gen_import_names.py`, `src/import_names.inc` | Maps the game's import NIDs to function names |
| Loader | `src/probe.c` | Loads the image and starts the game's original code |
| Runtime | `src/runtime_*.c` | PS4 system functions |
| Audio3d | `src/runtime_audio.c` | Mixes the bed and the sound objects of the Wwise sink into one output |
| Json2 | `src/runtime_json.c` | sce::Json values, parser and serializer (race results) |
| Red zones | `src/redzone.cpp`, `scripts/function_starts.py` | Keeps Windows exception records out of the game's red zones |
| Launcher | `launcher/` | Settings window and game start |
| GPU | `gpu/shadps4/` | shadPS4 video core (Vulkan) |

Many names (`gow3-probe.exe`, `GOW3_*` variables, the launcher) still come from the God of
War III port this project is based on. They will be renamed.

## Credits

This project is a fork of FelipePrado0's God of War III port, which comes from Supermedo's
Windows port of deadinside28's original:

- **FelipePrado0**: [gow3_pc](https://github.com/FelipePrado0/gow3_pc), the base of this project.
- **bc160201983**: [medievil_2019_pc_port](https://github.com/bc160201983/medievil_2019_pc_port),
  the first port of another game on that runtime. `pkg_extract.py`, `game_profiles.py` and
  `gen_import_names.py` come from it.
- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), the original
  native port.
- **Supermedo**: [bloodborne_pc for Windows](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4**: [shadPS4 fork](https://github.com/cuesta4/shadPS4/tree/eltutz) (branch `eltutz`):
  the GNM blend rewrite, depth/stencil state and image aliasing fixes.
- **shadPS4 team**: the [shadPS4](https://github.com/shadps4-emu/shadPS4) video core, the PKG
  format and the symbol names (`tools/aerolib.inl`).

Contributors to this port: **d4g** (render resolution patches, the Metro Park terrain fix),
**benhaslett** (game clock measurements, Linux build fix) and **iannitram** (PS4 FPU state).

Also used: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK,
[MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) and [Pillow](https://python-pillow.org).

## License

GNU GPL v2 or later ([LICENSE](LICENSE)). Third-party components keep their own licenses.
