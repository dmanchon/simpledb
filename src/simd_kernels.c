#include "simpledb/simd_kernels.h"
#include <stddef.h>
#include <stdint.h>

size_t simd_compress_i32(const int32_t *restrict src, const uint8_t *restrict mask, int32_t *restrict dst, size_t n) {
    size_t index = 0;

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
