# Parallel GPU command processing — design notes

Goal: remove the single-core bottleneck of the emulated GPU command processor
(`shadPS4:GpuCommandProcessor`, ~100% of one core while the rest of the CPU idles).

## Measurements (Bloodborne, Hunter's Nightmare, ~70 FPS, `BB_DCB_STATS=1`)

| | |
|---|---|
| Graphics command buffers submitted | 4200–5800/s, **~65–80 per frame** |
| Draws | ~110 000/s, ~1600 per frame |
| Draws per command buffer | mostly 10–99, max ~200 |
| Nested IndirectBuffer | none |
| State before the first draw | ~118 context + ~31 SH register dwords written; only 8% start with ClearState |

GPU thread profile after the single-thread work (perf, `cpu-clock:u`):
texture binding ~25%, pipeline selection ~19% (StageSpecialization build/compare,
sharp fetches), buffer binding ~14%, render targets ~8%, PM4 decode ~3%.

## Constraints

- Command buffers are not self-contained: register state is inherited, so a worker needs
  the register state at the start of its buffer (context + SH ranges are ~6–8 KiB; one
  snapshot per buffer is ~40 MB/s at 70 FPS).
- Draw preparation reads guest memory, not only registers: extended user data (EUD) and
  shader code. Earlier packets in the stream (DMA, WriteData, constant-engine dumps) can
  change that memory, so work done ahead of the GPU thread may see stale data.
- The caches (buffer, texture, pipeline) and per-draw scratch state (flags in `Image`,
  user data inside shared `Shader::Info`) are single-threaded by design.
- Image layout and barrier tracking assume one ordered stream.

## Plan

1. **Speculative draw preparation on workers, validated on the GPU thread.**
   A cheap serial pass records the register state at the start of each command buffer.
   Workers replay their buffer's register writes and, per draw, precompute the pure
   parts: pipeline key and stage specialization inputs, sharp fetches, texture and
   render-target descriptions, dynamic state values, vertex buffer ranges. Each result
   carries the flattened user data it was computed from. The GPU thread still decodes
   the stream and refreshes the flattened user data (cheap), compares it with the
   worker's copy, and uses the prepared draw only on a match; otherwise it computes
   as today. Correctness never depends on the workers.
   Prerequisite refactor: sharp consumers read user data through a view instead of
   the shared `Shader::Info` members, and per-draw scratch moves into a context struct.
2. **Thread-safe cache lookups.** Finding existing buffers/images/views moves to the
   workers; creation, uploads, barriers stay on the GPU thread.
3. **Parallel Vulkan recording.** Each worker records its own command buffer; they are
   executed in submission order with barriers at the seams.

Every step keeps a `BB_TOGGLE_FILE` bit so it can be switched off at run time and
compared by screenshot and frame rate.

## Portability

Must scale down to the Steam Deck (4 cores / 8 threads): worker count follows
`hardware_concurrency()`, no busy waiting when cores are scarce, no AVX-512.

## Results

Step 1 (draw preparation, 4 workers, toggle 8192), Hunter's Nightmare, same view:
71.5 FPS with prepared draws vs 64.1 without (+11.5%), identical screenshots.
97–98% of direct draws use the prepared pipeline; each `bb:DrawPrep` worker ~10% of a core.
The GPU thread is still ~90% busy: texture/buffer binding is the next target (step 2).
