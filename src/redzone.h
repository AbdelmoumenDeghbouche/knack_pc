// SPDX-License-Identifier: GPL-2.0-or-later
// bbport-windows: protection of the game's stack red zones from Windows exception dispatch.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The loader's segment record (probe.c Segment): image offset, size, flags (1 = executable). */
typedef struct {
    uint64_t address, size, flags;
} BbRedZoneSegment;

/* Reroutes the faultable memory accesses of the game's functions whose red zone holds data
 * through trampolines that move the stack pointer below it first (src/redzone.cpp). Reads
 * the function starts from functions.bin next to boot_file (scripts/function_starts.py); call it
 * after relocation, while the image is still writable. Only done when a probe shows this Windows
 * build writing exception records into the red zone; GOW3_RED_ZONE=1 always patches, =0 never. */
void bbport_red_zone_protect(const char* boot_file, unsigned char* image, uint64_t image_size,
                             const BbRedZoneSegment* segments, uint64_t segment_count);

#ifdef __cplusplus
}
#endif
