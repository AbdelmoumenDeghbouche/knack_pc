WipEout Omega Collection for PC (wipeout_pc)
============================================

An early version of a native PC port. Menus and races (HD and 2048) play; not every
event type has been tested yet.

No game files are included. You need your own copy of WipEout Omega Collection, dumped from
your own PS4, as a folder containing eboot.bin, sce_module and sce_sys. Tested: CUSA07671 (US),
version 01.00 with update 01.07, merged into one folder. CUSA05670 (EU) is listed but untested.

Requirements
- Windows 10 (1903 or newer) or Windows 11, 64-bit.
- A 64-bit processor with AVX: Intel Sandy Bridge (2011) or newer, AMD FX or newer.
  Older processors stop at start with a "STOP: this CPU lacks" line in the log.
- A graphics card with Vulkan 1.3 and an up-to-date driver.
  Nothing else: Python and the libraries are inside this folder.

Starting
1. Extract this folder anywhere, for example C:\Games\wipeout-windows (not inside the zip).
2. Open "WipEout.exe". If Windows shows "Windows protected your PC", click More info, then
   Run anyway: the program is not digitally signed.
3. Click "Choose the game folder", then Browse... next to Game folder, and select the folder
   with eboot.bin.
4. Back on Home, adjust the quick settings if you like, then click PLAY. The first start takes
   a little longer while the game image is prepared.

Settings
- Frame rate: the game runs on real time, so it can go above 60 FPS (up to 240). The Frame
  rate limit on Home sets the cap.
- Render resolution: 1920x1080 by default. 2560x1080 and 5120x2160 (21:9) are in the port's
  in-game menu, under Performance, and apply on the next start. The other sizes in that list
  come from the God of War III port and do nothing here yet. The HUD and menus stay 16:9 and
  look stretched at 21:9.
- "Native 4K rendering (no checkerboard)" on the Game page is on by default: from 3840 pixels
  wide the game renders at full resolution instead of checkerboard.

While playing
- Insert on the keyboard, or R3 + L2 on the controller, opens the port's menu: image,
  performance and the performance overlay.
- Saved pipelines load in the background; a small note shows the progress. New areas can
  stutter the first time while their shaders compile.
- Keyboard keys: Game -> Controls in the launcher.

Next time
- "Play WipEout.exe" starts the game straight away with the saved settings. Make a desktop
  shortcut to it (Advanced -> Desktop shortcut), or add it to Steam with "Add a Non-Steam Game".
- Advanced -> Check for updates downloads and installs a new version. Saves and settings are
  kept.

Data
- Saves, shader cache and the log: user\ next to WipEout.exe (keep it when updating).
- Settings: gow3.ini next to WipEout.exe; launcher options in %APPDATA%\wipeout-launcher.
- Generated files (prepared game image, patches): out\.

Known issues
- PS4 Pro mode is not available yet.
- Some event types and tracks have not been tested.

Problems
- The game does not open: check Advanced -> Log, and send user\last_run.log with your report.
  With a custom Saves folder, last_run.log is in that folder instead.
- Memory error (error 1450): increase the Windows page file. Win+R ->
  SystemPropertiesAdvanced -> Performance Settings -> Advanced -> Virtual memory Change ->
  set C: to 16384 MB initial, 24576 MB maximum -> Set -> OK, then restart your PC.
- RTSS/Afterburner and ReShade must target bin\gow3-probe.exe, not the launcher. Select
  Vulkan in the ReShade installer.
- Report problems here, with your GPU, driver, CPU and the log:
  https://github.com/AbdelmoumenDeghbouche/wipeout_pc/issues

Some file names (gow3-probe.exe, gow3.ini) still come from the God of War III port this
project is based on.

This project is not affiliated with Sony Interactive Entertainment, XDev, Clever Beans or EPOS
Game Studios. WipEout is a trademark of Sony Interactive Entertainment.
