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

Guest write faults (same view, toggle 65536): the game fills its per-frame buffers
sequentially and each 4 KiB page cost a protection fault — ~115k faults/s, ~20% of every
GXWorker and of the main thread spent in the kernel. Unprotecting the aligned 64 KiB window
around a fault: 16k faults/s, kernel time ~8%, **81.0 FPS vs 66.4** (+22%), identical frames.
The GPU command thread is back at ~100%: it is the limit again.

Rejected: "hot pages" (never re-protect pages written repeatedly, upload them on every
binding) — the set grew to ~14k pages, re-uploads dropped the frame rate to 33 FPS and a GPU
ring timeout followed. Left opt-in behind BB_HOT_PAGES=1.

Texture description cache 2-way/4096, same-target fast path, LRU touch skip, no per-texture
meta lookup: other outdoor view, 93.9 FPS; with the texture memos off (mask 1056) 65.6 FPS.
Close to the 100 Hz display cap (vblank-paced), so further gains need an uncapped test.

## Streaming stutter (BB_FRAME_STATS "Stall:" lines)

Running through new areas gave 60–170 ms frames (also in shadPS4). The GPU thread was busy
the whole frame, mostly in the kernel, uploading 50–400 MB of textures and buffers per frame.
Findings, in the order they were fixed:

1. Guest-to-staging copies ran on one thread (the recording thread, textures on the GPU
   thread). They now start at once on copy threads (`BbCopy::Async`, `bbport_copy.cpp`);
   small copies are batched per thread (one wakeup per ~512 KiB — one per copy cost 25% FPS).
   Guest-visible fences and queue submission wait for them (`Scheduler::WaitHostCopies`),
   which keeps the fix for UI flicker (the guest reused buffers before deferred copies ran).
2. Copies then ran at 0.2–0.4 GB/s per thread, almost all in the kernel: the first CPU access
   to a new staging block makes the kernel allocate and clear it (~2 ms per 16 MiB), and the
   staging pool freed blocks after 3 s idle, between streaming bursts. It now keeps 512 MiB
   (`BB_STAGING_KEEP_MB`), frees the rest after 30 s and populates 128 MiB at startup.
3. Write faults: a 256 KiB unprotect window (`BB_FAULT_WINDOW`) halves them again.
   `BB_UFFD=1` tracks writes with userfaultfd write-protection instead of mprotect (no
   address-space write lock, no mapping splits); read protection for readbacks still uses
   mprotect. It removes the mprotect time but did not change the stalls measurably; opt-in.
4. File reads into write-protected guest pages failed with EFAULT (kernel copies do not reach
   the fault handler); reads now touch each destination page first.

Result: stalls are mostly 40–50 ms (GPU thread ~30 ms of draw work plus ~12 ms of copies)
instead of 60–170 ms; the area load frame 350 ms instead of 430–760 ms.
