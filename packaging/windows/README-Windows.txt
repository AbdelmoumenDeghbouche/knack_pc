Knack for PC (knack_pc)
=======================

An early version of a native PC port. The first chapters play from start to end: fights,
super moves, story videos, sound and saves. Later chapters are not tested yet.

No game files are included. You need your own copy of Knack, dumped from your own PS4, as a
folder containing eboot.bin, sce_module and sce_sys. Tested: CUSA00068 (US) with update 01.10.
CUSA00006 and CUSA00202 (EU) are listed but untested.

Requirements
- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX: Intel Sandy Bridge (2011) or newer, AMD FX or newer.
  Older processors stop at start with a "STOP: this CPU lacks" line in the log.
- A graphics card with Vulkan 1.3 and an up-to-date driver.
- 16 GB of RAM is recommended: the game uses about 5 to 6 GB.
  Nothing else: Python and the libraries are inside this folder.

Starting
1. Extract this folder anywhere, for example C:\Games\knack-windows (not inside the zip).
2. Open "Knack.exe". If Windows shows "Windows protected your PC", click More info, then
   Run anyway: the program is not digitally signed.
3. Click "Choose the game folder", then Browse... next to Game folder, and select the folder
   with eboot.bin. If the 01.10 update is in its own folder, select it as the Update folder.
   You can also merge the update into the game folder.
4. Back on Home, adjust the quick settings if you like, then click PLAY. The first start takes
   a little longer while the game image is prepared.

Settings
- Frame rate: the game runs on real time, so it can go above 60 FPS (up to 120). The Frame
  rate limit on Home sets the cap.

While playing
- Insert on the keyboard, or R3 + L2 on the controller, opens the port's menu: image,
  performance and the performance overlay.
- New areas can stutter the first time while their shaders compile. Saved pipelines load in
  the background on the next start.
- Keyboard keys: Game -> Controls in the launcher.

Next time
- "Play Knack.exe" starts the game straight away with the saved settings. Make a desktop
  shortcut to it (Advanced -> Desktop shortcut), or add it to Steam with "Add a Non-Steam Game".
- Advanced -> Check for updates downloads and installs a new version. Saves and settings are
  kept.

Data
- Saves, shader cache and the log: user\ next to Knack.exe (keep it when updating).
- Settings: gow3.ini next to Knack.exe; launcher options in %APPDATA%\knack-launcher.
- Generated files (prepared game image): out\.

Known issues
- Dust puffs can show as squares with hard edges.
- Menu text can glitch for the first frames after the game starts.
- Memory use can grow during long play (about 6 GB after 20 minutes); not confirmed yet.
- PS4 Pro mode is not available.
- Later chapters and the EU versions are not tested.

Problems
- The game does not open: check Advanced -> Log, and send user\last_run.log with your report.
  With a custom Saves folder, last_run.log is in that folder instead.
- Memory error (error 1450): increase the Windows page file. Win+R ->
  SystemPropertiesAdvanced -> Performance Settings -> Advanced -> Virtual memory Change ->
  set C: to 16384 MB initial, 24576 MB maximum -> Set -> OK, then restart your PC.
- RTSS/Afterburner and ReShade must target bin\gow3-probe.exe, not the launcher. Select
  Vulkan in the ReShade installer.
- Report problems here, with your GPU, driver, CPU and the log:
  https://github.com/AbdelmoumenDeghbouche/knack_pc/issues

Some file names (gow3-probe.exe, gow3.ini) still come from the God of War III port this
project is based on.

This project is not affiliated with Sony Interactive Entertainment or Japan Studio. Knack is a
trademark of Sony Interactive Entertainment.
