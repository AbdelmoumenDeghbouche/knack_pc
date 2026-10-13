/* Loader side of the ISA rewrite (#33, #66): reads a G3ISA1 table (scripts/isa_rewrite.py), builds
 * one machine-code stub per site in an area next to the image and patches each site with a jmp
 * into its stub (or an int3). The stub saves the guest's registers, calls isa_emulate and jumps
 * back. Runs at load time, before the image is protected; libc is fine here. */
#ifndef GOW3_ISA_REWRITE_H
#define GOW3_ISA_REWRITE_H
#include <stddef.h>
#include <stdint.h>
#include "isa_emulate.h"
#include "cpu_check.h"

typedef struct {
    IsaSite *sites; uint64_t *hits;      /* hits[i] belongs to sites[i] */
    uint32_t count, unsupported;
    uint64_t image_size; uint8_t sha256[32];
    unsigned char *image, *stubs;        /* set by isa_rewrite_apply */
} IsaTable;
/* rewritten = jmp + relocated + int3; skipped and unsupported_needed are outside it. */
typedef struct { uint32_t rewritten, jmp, relocated, int3, skipped, unsupported_needed; } IsaReport;
enum { ISA_STUB_BYTES = 256 };

/* Parses a G3ISA1 file (spec 4.4). NULL and one stderr line ("ISA: <reason>") when invalid. */
IsaTable *isa_table_load(const char *path);
/* 0 when size and sha256 match the image. */
int isa_table_matches(const IsaTable *t, const unsigned char *image, uint64_t size);
size_t isa_stub_area_size(const IsaTable *t);           /* count * ISA_STUB_BYTES */
unsigned isa_site_set(const IsaSite *s);                 /* CPU_SET_* of the kind */
/* Rewrites the sites whose set is in `sets` (spec 5.4). Writes nothing and returns
 * unsupported_needed > 0 when a needed site is UNSUPPORTED. `stubs` has isa_stub_area_size bytes, RW. */
IsaReport isa_rewrite_apply(IsaTable *t, unsigned char *image, unsigned char *stubs, unsigned sets);

#endif
