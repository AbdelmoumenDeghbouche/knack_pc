# Packaged port: the prebuilt game binaries (build.sh first), the start-up scripts and the GTK4
# launcher, with their whole Nix closure and Mesa's Vulkan drivers. `bash packaging/appimage.sh`
# turns it into an AppImage (Steam Deck); `nix-build packaging` alone gives result/bin/bbport.
{ pkgs ? import <nixpkgs> { } }:
let
  lib = pkgs.lib;
  root = ./..;
  # Only what the package needs (the tree also holds builds, profiles and captures).
  wanted = [
    "run.sh" "prepare.py" "link_libc.py" "link_modules.py" "content_profile.py" "patches.py"
    "patches" "fsr4_shaders" "launcher" "out" "out/bb-probe" "out/gpu" "out/gpu/libbbgpu.so"
  ];
  src = builtins.path {
    name = "bbport-src";
    path = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in builtins.elem rel wanted
        || lib.any (dir: lib.hasPrefix (dir + "/") rel) [ "patches" "fsr4_shaders" "launcher" ];
  };
  python = pkgs.python3.withPackages (ps: [ ps.pygobject3 ]);
  # Mesa's own drivers: the host's (e.g. SteamOS /usr/lib) cannot load into this closure's glibc.
  icds = lib.concatMapStringsSep ":" (name: "${pkgs.mesa}/share/vulkan/icd.d/${name}")
    [ "radeon_icd.x86_64.json" "intel_icd.x86_64.json" ];
  # Environment the closure needs on any host: icon themes, SVG icon loader, fonts (the host's
  # font directories through fontconfig's default config) and a UTF-8 locale built into glibc.
  common = ''
      --prefix XDG_DATA_DIRS : ${pkgs.adwaita-icon-theme}/share:${pkgs.hicolor-icon-theme}/share:${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name} \
      --set-default GDK_PIXBUF_MODULE_FILE ${pkgs.librsvg}/${pkgs.gdk-pixbuf.moduleDir}.cache \
      --set-default FONTCONFIG_FILE ${pkgs.fontconfig.out}/etc/fonts/fonts.conf \
      --set-default LC_ALL C.UTF-8 \
  '';
in
pkgs.stdenv.mkDerivation {
  pname = "bbport";
  version = "0.1";
  inherit src;
  nativeBuildInputs = [ pkgs.patchelf pkgs.makeShellWrapper pkgs.wrapGAppsHook4 pkgs.gobject-introspection ];
  buildInputs = [ pkgs.gtk4 pkgs.libadwaita pkgs.adwaita-icon-theme pkgs.librsvg ];
  dontBuild = true;
  dontConfigure = true;
  # The binaries live under share/ (next to the scripts run.sh expects): strip them too, which
  # also drops the compiler and header paths their debug info would keep in the closure.
  stripDebugList = [ "share/bbport/bin" ];
  dontWrapGApps = true; # wrapped once below, together with the launcher's own variables
  installPhase = ''
    runHook preInstall
    d=$out/share/bbport
    mkdir -p $d/bin $out/bin
    cp run.sh prepare.py link_libc.py link_modules.py content_profile.py patches.py $d/
    cp -r patches fsr4_shaders launcher $d/
    install -m755 out/bb-probe $d/bin/bb-probe
    install -m755 out/gpu/libbbgpu.so $d/bin/libbbgpu.so
    # The development rpath points at the build tree: the library is next to the binary now.
    rpath=$(patchelf --print-rpath $d/bin/bb-probe | tr ':' '\n' | grep '^/nix/store' | paste -sd:)
    patchelf --set-rpath "$d/bin:$rpath" $d/bin/bb-probe
    runHook postInstall
  '';
  postFixup = ''
    makeShellWrapper ${python}/bin/python3 $out/bin/bbport \
      "''${gappsWrapperArgs[@]}" \
      ${common}      --add-flags $out/share/bbport/launcher/bbport_launcher.py \
      --set BB_PREBUILT 1 \
      --set PYTHON ${pkgs.python3}/bin/python3 \
      --set-default VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.util-linux ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/bbport}; mkdir -p "$BB_DATA_DIR"'
    # The game alone, without the launcher (settings from the data directory's bbport.ini).
    makeShellWrapper ${pkgs.bash}/bin/bash $out/bin/bbport-game \
      ${common}      --add-flags $out/share/bbport/run.sh \
      --set BB_PREBUILT 1 \
      --set PYTHON ${pkgs.python3}/bin/python3 \
      --set-default VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/bbport}; mkdir -p "$BB_DATA_DIR"'
  '';
  meta.mainProgram = "bbport";
}
