#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "simpledb/allocator.h"
#include "simpledb/slice.h"

// selection_vector_t is a semantic type alias for buffer_i32_t
typedef buffer_i32_t selection_vector_t;

static inline selection_vector_t* selection_vector_create(size_t capacity) {
    return buffer_i32_create(capacity);
}

static inline void selection_vector_free(selection_vector_t* sel) {
    buffer_i32_free(sel);
}

static inline void selection_vector_clear(selection_vector_t* sel) {
    if (sel) sel->size = 0;
}

static inline int32_t selection_vector_at(const selection_vector_t* sel, size_t idx) {
    return sel->data[idx];
}

// Out-of-line Vectorized Query Primitives (Implemented in src/filter.c)

// Branchless Filter Kernel: Appends row indices where src[i] > target
bool filter_gt_i32(slice_i32_t src, int32_t target, selection_vector_t* out_sel);

// Zero-Copy Gather Operator: Extracts values at selected row indices into out_buf
bool gather_i32(slice_i32_t src, const selection_vector_t* sel, buffer_i32_t* out_buf);
