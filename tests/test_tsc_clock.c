/* The guest's TSC: sceKernelReadTsc and sceKernelGetTscFrequency (src/runtime_kernel.c). */
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/runtime_kernel.c"

/* Neighbours of runtime_kernel.c that the clocks never call. */
int32_t *runtime_errno(void) { static int32_t value; return &value; }
void runtime_report(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static double seconds(uint64_t ticks, uint64_t hz) { return (double)ticks / (double)hz; }

int main(void) {
    const uint64_t hz = tsc_frequency();
    assert(hz > 100000000);
    /* sceKernelReadTsc is the clock the game's inline rdtsc reads (WipEout: 125 places). */
    const uint64_t a = __rdtsc(), b = read_tsc(), c = __rdtsc();
    assert(a <= b && b <= c);
    /* The frequency is the TSC's: a 200 ms nap reads the same on the TSC and the process
       counter, within 1 ms (a made-up 1 GHz on a 2.4 GHz TSC read 480 ms). */
    const uint64_t tsc0 = read_tsc(), counter0 = process_time_counter();
    sleep_ns(200000000);
    const uint64_t tsc1 = read_tsc(), counter1 = process_time_counter();
    const double tsc_s = seconds(tsc1 - tsc0, hz), counter_s = seconds(counter1 - counter0, 1000000000);
    assert(counter_s > 0.15 && fabs(tsc_s - counter_s) < 0.001);
    puts("tsc-clock-test ok");
    return 0;
}
