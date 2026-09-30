#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
if [[ ${1:-} == --software ]]; then
    shift
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then
        for candidate in /run/opengl-driver/share/vulkan/icd.d/lvp_icd*.json /usr/share/vulkan/icd.d/lvp_icd*.json; do
            if [[ -f $candidate ]]; then export VK_DRIVER_FILES=$candidate; break; fi
        done
    fi
    if [[ -z ${VK_DRIVER_FILES:-} ]]; then echo 'Lavapipe not found; set VK_DRIVER_FILES.' >&2; exit 1; fi
    export VK_LOADER_LAYERS_DISABLE='~implicit~'
fi
if [[ -z ${BB_IN_NIX_SHELL:-} ]] && ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3; } && command -v nix-shell >/dev/null; then
    args=''; if (( $# )); then args=$(printf '%q ' "$@"); fi
    exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash run.sh $args"
fi
if [[ -z ${PYTHON:-} ]]; then
    PYTHON=$(command -v python3 || true)
    if [[ -z $PYTHON ]]; then
        for candidate in /nix/store/*-python3-*/bin/python3; do
            if [[ -x $candidate ]]; then PYTHON=$candidate; break; fi
        done
    fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
"$PYTHON" prepare.py ../CUSA03173
"$PYTHON" link_libc.py ../CUSA03173
"$PYTHON" link_modules.py ../CUSA03173
"$PYTHON" content_profile.py ../CUSA03173 --sku "${BB_CONTENT_SKU:-full}"
# Dynamic scene resolution scaling now works on all GPUs (fallback: clear UI depth
# instead of blit when D32S8 blit unsupported). The old startup resolution patch
# is kept as BB_RENDER_RES for explicit overrides and compatibility testing.
if [[ ${BB_AUTO_RENDER_RES:-} == 1 ]]; then
    unset BB_RENDER_RES BB_AUTO_RENDER_RES
fi
# BB_RENDER_RES=WxH explicitly overrides dynamic scaling (for testing/debugging).
# Without it, all GPUs use live preset switching.
# Frame rate: BB_FPS=uncap (default; delta-time patch, vblank follows the display),
# 60/90 (fixed-timestep patches) or 30 (unpatched). BB_PATCHES adds patch names ("a;b").
fps=${BB_FPS:-uncap}
# Supported GPUs scale renderer targets at run time. BB_RENDER_RES=WxH keeps the
# explicit guest-resolution patch for compatibility and debugging.
"$PYTHON" patches.py --fps "$fps" --extra "${BB_PATCHES:-}" --settings "${BB_CONFIG:-bbport.ini}" --render-res "${BB_RENDER_RES:-}"
if [[ -z ${BB_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export BB_VBLANK_HZ=0 ;; 90) export BB_VBLANK_HZ=90 ;; *) export BB_VBLANK_HZ=60 ;; esac
fi
bash build.sh
exec out/bb-probe out/boot-linked.bin --content-profile out/content.bin --patches out/patches.bin --app0 ../CUSA03173 --user "${BB_USER_DIR:-user}" --timeout "${BB_TIMEOUT:-0}" "$@"
