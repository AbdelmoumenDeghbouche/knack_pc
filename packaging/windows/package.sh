#!/usr/bin/env bash
# Builds dist/bbport-windows/ (and dist/bbport-windows.zip): Bloodborne.exe (the launcher, frozen
# with PyInstaller so players need no Python), bb-probe.exe with the MSYS2 CLANG64 DLLs it needs,
# the preparation scripts and run.py. Run from an MSYS2 CLANG64 shell after `bash build.sh`.
# Freezing uses a Windows Python 3.10+ (python.org; WINPYTHON overrides) and a private venv in
# out/pyenv with PyInstaller. FSR 4 assets in fsr4_shaders/ are included when present.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
[[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

# A Windows Python (not MSYS2's) for PyInstaller.
python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
# Windows Python needs USERPROFILE (some MSYS2 shells start without it).
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
if [[ ! -x out/pyenv/Scripts/python.exe ]]; then
    "$python" -m venv out/pyenv
fi
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller
# The scripts run inside Bloodborne.exe (--script): the standard modules they import come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil struct tempfile xml.etree.ElementTree \
              urllib.request ctypes.wintypes; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name Bloodborne --icon "$(cygpath -w "$PWD/launcher/bloodborne.ico")" --distpath out/pyi-dist \
    --workpath out/pyi-work --specpath out/pyi-work --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/bbport_launcher_win.py")"

dest=dist/bbport-windows
# A fresh folder: a previous package may hold generated files from test runs. Saves played from
# it (user/) are kept aside and put back after the zip is made, so they never ship.
rm -rf -- out/package-user
if [[ -d $dest/user ]]; then mv -- "$dest/user" out/package-user; fi
rm -rf -- dist/bbport-windows
mkdir -p "$dest/bin" "$dest/launcher"
cp -r out/pyi-dist/Bloodborne/. "$dest/"
cp launcher/bloodborne.ico launcher/bloodborne.png "$dest/launcher/"
# The executables without debug information (out/ keeps the symbols for crash reports).
for exe in bb-probe.exe bb-gpu-capabilities.exe; do
    llvm-strip --strip-debug -o "$dest/bin/$exe" "out/$exe"
done
# Every DLL the executables load from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/bb-probe.exe" "$dest/bin/bb-gpu-capabilities.exe" |
    awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
        cp -u "$dll" "$dest/bin/"
    done
cp -r scripts patches "$dest/"
cp run.py LICENSE README.md packaging/windows/README-Windows.txt "$dest/"
if [[ -d fsr4_shaders ]]; then cp -r fsr4_shaders "$dest/"; fi
find "$dest" -name __pycache__ -prune -exec rm -r {} +
(cd dist && rm -f bbport-windows.zip && powershell -NoProfile -Command \
    "Compress-Archive -Path bbport-windows -DestinationPath bbport-windows.zip")
if [[ -d out/package-user ]]; then mv -- out/package-user "$dest/user"; fi
du -sh "$dest" dist/bbport-windows.zip
