# Day 4: Build System, Compiler Optimizations & POSIX Micro-Benchmarking

> **Day 4 Branch**: View the exact code for this day at [dmanchon/simpledb (`day4`)](https://github.com/dmanchon/simpledb/tree/day4) ([diff from `day3`](https://github.com/dmanchon/simpledb/compare/day3...day4)).

Empirical throughput benchmarking is essential to evaluate columnar hardware scan performance. In `simpledb`, micro-benchmarking relies on POSIX monotonic timers and compiler memory barriers to measure memory scan bandwidth in gigabytes per second (GB/s).

---

## 1. High-Precision POSIX Monotonic Timers

Standard wall-clock functions like `gettimeofday` are subject to system time adjustments and NTP synchronization, while `clock()` measures total process CPU time rather than elapsed real time.

For precise hardware measurements, `simpledb` uses `clock_gettime(CLOCK_MONOTONIC)` to record elapsed nanoseconds.

```c
// [include/simpledb/benchmark.h]
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
  #define PREVENT_DCE(var) __asm__ __volatile__("" : : "r"(var))
#else
  #define PREVENT_DCE(var) do { volatile __typeof__(var) _dummy = (var); (void)_dummy; } while(0)
#endif

static inline uint64_t benchmark_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void benchmark_report(const char *name, size_t bytes, uint64_t elapsed_ns) {
    double seconds = (double)elapsed_ns / 1e9;
    double gb_per_sec = (seconds > 0) ? (((double)bytes / seconds) / 1e9) : 0.0;
    double us = (double)elapsed_ns / 1e3;
    printf("[%s] Scanned %.2f MB in %.2f us (%.6f sec) | Throughput: %.2f GB/s\n",
           name, (double)bytes / 1e6, us, seconds, gb_per_sec);
}
```

---

## 2. Instruction-Level Parallelism and Loop Unrolling

Naive reduction loops can stall CPU pipelines due to Read-After-Write (RAW) data dependencies.

```c
// Single accumulator dependency chain
int64_t sum = 0;
for (size_t i = 0; i < len; i++) {
    sum += data[i]; // Addition i+1 must wait for addition i to complete
}
```

Because integer addition instructions carry a latency of 3 to 4 clock cycles, a single accumulator variable forces execution units to wait for the previous iteration's result.

Unrolling the loop across four independent accumulators breaks the dependency chain:

```c
int64_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
const int32_t *data = buf->data;
size_t i = 0;

for (; i + 3 < num_elements; i += 4) {
    sum0 += data[i + 0];
    sum1 += data[i + 1];
    sum2 += data[i + 2];
    sum3 += data[i + 3];
}
for (; i < num_elements; i++) {
    sum0 += data[i];
}
int64_t total_sum = sum0 + sum1 + sum2 + sum3;

PREVENT_DCE(total_sum);
```

By decoupling the arithmetic operations into `sum0`, `sum1`, `sum2`, and `sum3`, out-of-order execution pipelines dispatch all four additions concurrently in the same clock cycle.

---

## 3. Compiler Dead Code Elimination Barriers

When compiling with high optimization flags (`-O3`), compiler passes detect variables that are not written to disk or printed to `stdout`, optimizing away the reduction loop entirely.

Declaring loop accumulators as `volatile` inside the loop body forces memory writes back to stack memory on every iteration, significantly reducing memory scanning bandwidth.

The `PREVENT_DCE(var)` macro expands to `__asm__ __volatile__("" : : "r"(var))` under GCC and Clang. This signals to the compiler optimizer that `total_sum` is consumed by an assembly block, preserving the vector loop while emitting zero additional machine instructions.

---

## 4. CMake Compiler Optimization Configuration

Compiler flags in `CMakeLists.txt` enable native SIMD vectorization and Link-Time Optimization (LTO):

```cmake
add_executable(bench_db benchmarks/bench_db.c)
target_link_libraries(bench_db PRIVATE simpledb_lib)
if(NOT MSVC)
    target_compile_options(bench_db PRIVATE -O3 -march=native -ffast-math)
endif()
```

---

## 5. Architectural Execution Characteristics and Optimization Impact

The scanning throughput achieved by a columnar reduction kernel depends directly on compiler optimization flags and memory access patterns.

| Execution Technique | Compiler Flags | Performance Scaling Impact | Dominant Hardware Bottleneck |
| :- | :- | :- | :- |
| Volatile Accumulation | `-O3` (Volatile `sum`) | Baseline (Slowest) | Stack Write-Back Memory Bus Locks |
| Standard Scalar Loop | `-O2` | ~4x - 5x Speedup | RAW Loop Addition Latency Chains |
| 4-Way Unrolled SIMD | `-O3 -march=native` | ~8x - 10x Speedup | Out-of-Order Instruction Dispatch Limits |
| Zero-Copy mmap SIMD | `-O3 -march=native` | Peak Hardware Speedup | Hardware Bus Bandwidth Limit (~40-80 GB/s) |

---

## 6. Final Implementation (`benchmarks/bench_db.c`)

```c
#include "simpledb/allocator.h"
#include "simpledb/mmap_file.h"
#include "simpledb/benchmark.h"
#include <stdio.h>
#include <stdlib.h>

static void benchmark_simd_unrolled_scan(size_t num_elements) {
    buffer_i32_t *buf = buffer_i32_create(num_elements);
    if (!buf) return;
    buf->size = num_elements;

    for (size_t i = 0; i < num_elements; i++) {
        buf->data[i] = (int32_t)(i & 0xFF);
    }

    size_t total_bytes = num_elements * sizeof(int32_t);
    uint64_t start = benchmark_now_ns();

    int64_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
    const int32_t *data = buf->data;
    size_t i = 0;

    // 4-Way SIMD Loop Unrolling for maximum Instruction-Level Parallelism (ILP)
    for (; i + 3 < num_elements; i += 4) {
        sum0 += data[i + 0];
        sum1 += data[i + 1];
        sum2 += data[i + 2];
        sum3 += data[i + 3];
    }
    for (; i < num_elements; i++) {
        sum0 += data[i];
    }
    int64_t total_sum = sum0 + sum1 + sum2 + sum3;

    // Prevent compiler Dead Code Elimination (DCE) AFTER loop without degrading vectorization!
    PREVENT_DCE(total_sum);

    uint64_t end = benchmark_now_ns();
    uint64_t elapsed_ns = end - start;

    benchmark_report("4-Way Unrolled SIMD Scan", total_bytes, elapsed_ns);

    buffer_i32_free(buf);
}

int main(void) {
    printf("=====================================================\n");
    printf("   simpledb Day 4 Micro-Benchmark Hardware Scanner   \n");
    printf("=====================================================\n\n");

    printf("[Running 10M Element Scan (40 MB Buffer)]...\n");
    benchmark_simd_unrolled_scan(10000000);

    printf("\n[Running 50M Element Scan (200 MB Buffer)]...\n");
    benchmark_simd_unrolled_scan(50000000);

    printf("\n[Running 100M Element Scan (400 MB Buffer)]...\n");
    benchmark_simd_unrolled_scan(100000000);

    printf("\n=====================================================\n");
    printf("Benchmark suite completed successfully.\n");
    return 0;
}
```

---

> **Day 4 Branch**: View the exact code for this day at [dmanchon/simpledb (`day4`)](https://github.com/dmanchon/simpledb/tree/day4) ([diff from `day3`](https://github.com/dmanchon/simpledb/compare/day3...day4)).
