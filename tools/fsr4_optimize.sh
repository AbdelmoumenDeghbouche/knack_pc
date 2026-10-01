#!/usr/bin/env bash
# tools/fsr4_optimize.sh: builds RDNA3-friendly variants of the FSR 4 v07 post passes into
# fsr4_shaders/opt/ (vk_fsr4.cpp prefers them; BB_FSR4_OPT=0 uses the originals). Each post pass
# is decompiled (spirv-cross), rewritten by fsr4_post_lds.pl (stores through shared memory,
# bit-exact) and compiled again (glslang). Needs spirv-cross and glslangValidator (shell.nix).
# tools/fsr4_verify.sh checks every variant against the original with the benchmark.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
src=${BB_FSR4_DIR:-fsr4_shaders}
dest=$src/opt
mkdir -p "$dest"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
count=0
for spv in "$src"/fsr4_model_v07_i8_*_post.spv; do
    name=$(basename "$spv")
    out=$dest/$name
    # Rebuilt when the original or the rewrite changed.
    if [[ -s $out && $out -nt $spv && $out -nt tools/fsr4_post_lds.pl ]]; then continue; fi
    spirv-cross "$spv" --vulkan-semantics --output "$tmp/post.comp"
    perl tools/fsr4_post_lds.pl < "$tmp/post.comp" > "$tmp/post_lds.comp"
    glslangValidator -V --target-env vulkan1.3 -S comp "$tmp/post_lds.comp" -o "$tmp/out.spv" >/dev/null
    mv "$tmp/out.spv" "$out"
    count=$((count + 1))
done
echo "FSR 4 optimized post passes: $count built in $dest"
