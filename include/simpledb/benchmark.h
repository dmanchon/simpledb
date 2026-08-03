#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

// Macro preventing compiler Dead Code Elimination (DCE) without degrading SIMD loop vectorization
#if defined(__GNUC__) || defined(__clang__)
  #define PREVENT_DCE(var) __asm__ __volatile__("" : : "r"(var))
#else
  #define PREVENT_DCE(var) do { volatile __typeof__(var) _dummy = (var); (void)_dummy; } while(0)
#endif

// Returns current monotonic clock time in nanoseconds
static inline uint64_t benchmark_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Prints hardware scanning throughput in GB/s
static inline void benchmark_report(const char *name, size_t bytes, uint64_t elapsed_ns) {
    double seconds = (double)elapsed_ns / 1e9;
    double gb_per_sec = (seconds > 0) ? (((double)bytes / seconds) / 1e9) : 0.0;
    double us = (double)elapsed_ns / 1e3;
    printf("[%s] Scanned %.2f MB in %.2f us (%.6f sec) | Throughput: %.2f GB/s\n",
           name, (double)bytes / 1e6, us, seconds, gb_per_sec);
}
