#include "simpledb/filter.h"
#include "simpledb/allocator.h"
#include "simpledb/slice.h"
#include <stddef.h>
#include <stdint.h>

void selection_vector_ensure_capacity(selection_vector_t *sel, size_t needed_capacity) {
    if (!sel || sel->capacity >= needed_capacity)
        return;
    size_t new_cap = (sel->capacity == 0) ? 16 : sel->capacity;
    while (new_cap < needed_capacity) {
        new_cap *= 2; // Keep doubling until capacity is big enough!
    }
    int32_t *new_data = (int32_t *)aligned_malloc(new_cap * sizeof(int32_t));
    if (sel->data) {
        if (sel->size > 0) {
            memcpy(new_data, sel->data, sel->size * sizeof(int32_t));
        }
        aligned_free(sel->data);
    }
    sel->data = new_data;
    sel->capacity = new_cap;
}

bool filter_gt_i32(slice_i32_t src, int32_t target, selection_vector_t *out_sel) {
    if (!src.data || !out_sel)
        return false;

    // worst case scenario: all rows match, need room for existing + new
    selection_vector_ensure_capacity(out_sel, out_sel->size + src.len);

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

    selection_vector_ensure_capacity(out_buf, sel->size);

    for (size_t i = 0; i < sel->size; i++) {
        buffer_i32_push(out_buf, slice_i32_at(src, (size_t)sel->data[i]));
    }
    return true;
}
