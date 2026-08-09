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
