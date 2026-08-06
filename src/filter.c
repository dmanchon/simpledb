#include "simpledb/filter.h"
#include "simpledb/allocator.h"
#include "simpledb/slice.h"
#include <stddef.h>
#include <stdint.h>

bool filter_gt_i32(slice_i32_t src, int32_t target, selection_vector_t *out_sel) {
    if (!src.data || !out_sel)
        return false;

    // worst case scenario: all rows match, need room for existing + new
    buffer_i32_ensure_capacity(out_sel, out_sel->size + src.len);

    for (size_t i = 0; i < src.len; i++) {
        bool match = src.data[i] > target;
        out_sel->data[out_sel->size] = (int32_t)i;
        out_sel->size += match;
    }
    return true;
}

bool gather_i32(slice_i32_t src, const selection_vector_t *sel, buffer_i32_t *out_buf) {
    if (!src.data || !sel || !out_buf)
        return false;

    buffer_i32_ensure_capacity(out_buf, sel->size);

    for (size_t i = 0; i < sel->size; i++) {
        buffer_i32_push(out_buf, slice_i32_at(src, (size_t)sel->data[i]));
    }
    return true;
}
