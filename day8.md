
In APL and K/q, the fundamental way to filter data isn't with `for` loops and `if` statements—it's with an operator called **Compress** (often written as `X / Y` or `where X`). 

When analyzing millions of rows, traditional `if` statements ruin performance because of **branch misprediction**. If the CPU mispredicts whether a row matches a filter, it has to flush its entire instruction pipeline, wasting 15-20 clock cycles per mistake.

Today, we will build a **SIMD-friendly Compress kernel** in pure C11. By writing tight, branchless loops, we allow modern compilers (Clang/GCC) to automatically vectorize our code into ARM Neon or Intel AVX2 intrinsics, processing 8 to 16 elements per clock cycle!

---

## The underlying mechanism of Pandas, ClickHouse, and DuckDB

If you have ever used Python's Pandas or NumPy, you have already used these exact primitives. When you write Python code like this:
```python
filtered_df = df[df['score'] > 90]
```
Python itself is far too slow to evaluate this row-by-row. Instead, Pandas delegates the work to a highly optimized C/C++ kernel exactly like the one we are building today. 
1. It generates a boolean array (`1` if score > 90, `0` otherwise).
2. It calls a native C `compress` function to densely pack the matching rows.

This same architecture—vectorized block execution and branchless SIMD compress kernels—is the exact reason why **ClickHouse** and **DuckDB** are so ferociously fast. They don't process data row-by-row; they process data in columnar chunks (usually 1,024 to 8,192 elements at a time), blasting them through tight C/C++ SIMD loops. What we are building today is the literal engine block that gives those systems their raw power.

---

## 1. The SIMD Kernels Header

We will define two fundamental APL primitives:
1. **Compress**: Takes a boolean mask (1s and 0s) and a source array. It "compresses" the source array, extracting only the elements where the mask is `1`, writing them contiguously to a destination array.
2. **Sum Reduction**: A heavily optimized loop that sums an array of integers.

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

---

## 2. Why `restrict`?

Notice the `restrict` keyword in our pointer arguments. This is a critical C11 feature for high-performance databases. 

In C, the compiler always assumes that two pointers *might* point to the exact same memory address (a problem called **pointer aliasing**). If `src` and `dst` overlap, the compiler cannot safely use SIMD instructions to load/store multiple values at once, because writing to `dst` might accidentally overwrite the next values to be read from `src`.

By adding `restrict`, we promise the compiler: *"I guarantee these pointers do not overlap."* This single keyword gives Clang/GCC the green light to deploy aggressive SIMD auto-vectorization.

### `simd_kernels.c`
```c
#include "simpledb/simd_kernels.h"
#include <stddef.h>

size_t simd_compress_i32(const int32_t *restrict src, const uint8_t *restrict mask, int32_t *restrict dst, size_t n) {
    size_t index = 0;

    // This loop is perfectly branchless.
    // The CPU doesn't have to guess whether the `if` statement is true.
    // Clang/GCC will heavily vectorize this using SIMD instructions.
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

---

## 3. The 4-Million Row Test Harness

Here is the test harness for Day 8. We will generate 4,000,000 random integers and a randomized boolean mask, and then pass them through our `simd_compress_i32` kernel. We will also use POSIX `clock_gettime` to measure just how fast a branchless C11 array primitive can execute!

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
    
    // Generate random data
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
