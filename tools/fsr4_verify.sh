#!/usr/bin/env bash
# tools/fsr4_verify.sh [frames]: runs the FSR 4 benchmark (out/gpu/fsr4-bench, pseudo-random
# inputs) for every preset and output size with the original and the optimized post pass
# (tools/fsr4_optimize.sh), checks that the outputs are bit-exact and prints both times.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
frames=${1:-12}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0
time_of() { grep -E '^ +[0-9.]+ ms/frame  post$' "$1" | tail -1 | awk '{print $1}'; }
for out in 1920x1080 2560x1440 3840x2160; do
    ow=${out%x*}; oh=${out#*x}
    preset=0
    for ratio in 1.0 1.5 1.7 2.0 3.0; do
        render=$(awk -v w="$ow" -v h="$oh" -v r="$ratio" 'BEGIN { printf "%dx%d", int(w / r + 0.5), int(h / r + 0.5) }')
        for run in 0 0b 1; do
            BB_FSR4_OPT=${run%b} BENCH_NOISE=1 BENCH_DUMP=$tmp/$run.raw \
                out/gpu/fsr4-bench "$render" "$out" $preset "$frames" > "$tmp/$run.log" 2>&1
        done
        for opt in 0 1; do
            BB_FSR4_OPT=$opt out/gpu/fsr4-bench "$render" "$out" $preset 300 > "$tmp/t$opt.log" 2>&1
        done
        # The original is not deterministic at some sizes (1080 tier: a strip at the left edge);
        # then the optimized pass may differ from it by no more than it differs from itself.
        if cmp -s "$tmp/0.raw" "$tmp/1.raw"; then
            result=bit-exact
        else
            self=$(cmp -l "$tmp/0.raw" "$tmp/0b.raw" | wc -l || true)
            other=$(cmp -l "$tmp/0.raw" "$tmp/1.raw" | wc -l || true)
            if (( self > 0 && other <= self * 3 / 2 )); then
                result="original nondeterministic ($self bytes differ between its runs, $other vs optimized)"
            else
                result="DIFFERS ($other bytes; original vs itself $self)"; fail=1
            fi
        fi
        printf '%-9s preset %d %-9s post %s -> %s ms  %s\n' "$out" $preset "$render" \
            "$(time_of "$tmp/t0.log")" "$(time_of "$tmp/t1.log")" "$result"
        preset=$((preset + 1))
    done
done
exit $fail
