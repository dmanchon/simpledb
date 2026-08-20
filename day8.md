# Day 8: SIMD APL Vector Primitives (Compress & Reduction)

In APL and K/q, the fundamental way to filter data does not rely on `for` loops and `if` statements. It utilizes an operator called **Compress** (often written as `X / Y` or `where X`). 

When analyzing millions of rows, traditional `if` statements degrade performance because of **branch misprediction**. If the CPU mispredicts whether a row matches a filter, it must flush its entire instruction pipeline, wasting 15 to 20 clock cycles per misprediction.

This section covers building a **SIMD-friendly Compress kernel** in C11. Utilizing the branchless SIMD foundations established in Day 5, we allow modern compilers (Clang/GCC) to automatically vectorize code into ARM Neon or Intel AVX2 intrinsics, processing 8 to 16 elements per clock cycle.

## Columnar Execution in Pandas, ClickHouse, and DuckDB

Vectorized systems like Python's Pandas or NumPy use these primitives. For example:
```python
filtered_df = df[df['score'] > 90]
```
Rather than evaluating this row-by-row in Python, Pandas delegates the work to a highly optimized C/C++ kernel similar to the one implemented here. 
1. It generates a boolean array (`1` if score > 90, `0` otherwise).
2. It calls a native C `compress` function to densely pack the matching rows.

This architecture (vectorized block execution and branchless SIMD compress kernels) is fundamental to systems like **ClickHouse** and **DuckDB**. They process data in columnar chunks (usually 1,024 to 8,192 elements at a time) through compiled C/C++ SIMD loops.

## 1. The SIMD Kernels Header

The implementation defines two fundamental APL primitives:
1. **Compress**: Takes a boolean mask (1s and 0s) and a source array. It extracts only the elements where the mask is `1`, writing them contiguously to a destination array.
2. **Sum Reduction**: An optimized loop that sums an array of integers.

### `simd_kernels.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>

// APL Compress (X / Y): Extracts elements from `src` into `dst` where `mask` is 1.
// Returns the number of elements written to `dst`.
// Note: `mask` is an array of bytes (1 for true, 0 for false) for auto-vectorization.
size_t simd_compress_i32(const int32_t* restrict src, 
                         const uint8_t* restrict mask, 
                         int32_t* restrict dst, 
                         size_t n);

// APL Sum Reduction (+/ X): Computes the sum of all elements in the array.
int64_t simd_sum_i32(const int32_t* restrict src, size_t n);
```

## 2. Why `restrict`?

The `restrict` keyword in the pointer arguments is a necessary C11 feature for high-performance execution. 

In C, the compiler assumes that two pointers might point to the exact same memory address, known as **pointer aliasing**. If `src` and `dst` overlap, the compiler cannot safely use SIMD instructions to load or store multiple values at once. Writing to `dst` might overwrite the next values to be read from `src`.

By adding `restrict`, the compiler is guaranteed that the pointers do not overlap. This keyword allows Clang/GCC to deploy aggressive SIMD auto-vectorization.

### `simd_kernels.c`
```c
#include "simpledb/simd_kernels.h"
#include <stddef.h>

size_t simd_compress_i32(const int32_t *restrict src, const uint8_t *restrict mask, int32_t *restrict dst, size_t n) {
    size_t index = 0;

    // This loop is branchless.
    // Clang/GCC will vectorize this using SIMD instructions.
    for (size_t i = 0; i < n; i++) {
        dst[index] = src[i];
        index += mask[i];
    }

    return index;
}

int64_t simd_sum_i32(const int32_t* restrict src, size_t n) {
    int64_t result = 0;
    for (size_t i =0; i < n; i++) {
        result += src[i];
    }
    return result;
}
```

## 3. The 4-Million Row Test Harness

This test harness generates 4,000,000 random integers and a randomized boolean mask, then passes them through the `simd_compress_i32` kernel. It uses POSIX `clock_gettime` to measure the execution time of the branchless C11 array primitive.

```c
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include "simpledb/allocator.h"
#include "simpledb/simd_kernels.h"

#define NUM_ELEMENTS 4000000

// Helper for high-res timing
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000.0) + (ts.tv_nsec / 1000000.0);
}

void test_simd_primitives() {
    printf("Testing SIMD APL Primitives (4 Million Elements)...\n");
    
    int32_t* src = aligned_malloc(NUM_ELEMENTS * sizeof(int32_t));
    uint8_t* mask = aligned_malloc(NUM_ELEMENTS * sizeof(uint8_t));
    int32_t* dst = aligned_malloc(NUM_ELEMENTS * sizeof(int32_t));
    
    assert(src && mask && dst);
    
    for (size_t i = 0; i < NUM_ELEMENTS; i++) {
        src[i] = rand() % 1000;
        mask[i] = (rand() % 100) < 25 ? 1 : 0; // 25% selectivity
    }
    
    double start = get_time_ms();
    size_t written = simd_compress_i32(src, mask, dst, NUM_ELEMENTS);
    double end = get_time_ms();
    
    printf("Compress extracted %zu elements in %.3f ms\n", written, end - start);
    
    start = get_time_ms();
    int64_t total_sum = simd_sum_i32(dst, written);
    end = get_time_ms();
    
    printf("Sum Reduction (Total: %lld) took %.3f ms\n", total_sum, end - start);
    
    aligned_free(src);
    aligned_free(mask);
    aligned_free(dst);
    
    printf("SIMD Primitives tests passed!\n");
}

int main() {
    test_simd_primitives();
    printf("All Day 8 tests passed!\n");
    return 0;
}
```
