#include "simpledb/filter.h"
#include "simpledb/allocator.h"
#include "simpledb/slice.h"
#include "simpledb/string_dict.h"
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


bool filter_lt_i32(slice_i32_t src, int32_t target, selection_vector_t *out_sel) {
    if (!src.data || !out_sel)
        return false;

    // worst case scenario: all rows match, need room for existing + new
    buffer_i32_ensure_capacity(out_sel, out_sel->size + src.len);

    for (size_t i = 0; i < src.len; i++) {
        bool match = src.data[i] < target;
        out_sel->data[out_sel->size] = (int32_t)i;
        out_sel->size += match;
    }
    return true;
}

bool filter_gt_i32_sel(slice_i32_t src, int32_t target, const selection_vector_t* in_sel, selection_vector_t* out_sel) {
    if (!src.data || !in_sel || !out_sel) return false;

    buffer_i32_ensure_capacity(out_sel, out_sel->size + in_sel->size);

    for (size_t i = 0; i < in_sel->size; i++) {
        int32_t row_idx = in_sel->data[i];
        bool match = src.data[row_idx] > target;
        out_sel->data[out_sel->size] = row_idx;
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

bool filter_eq_i32(slice_i32_t src, int32_t target, selection_vector_t* out_sel) {
    if (!src.data || !out_sel) return false;

    buffer_i32_ensure_capacity(out_sel, out_sel->size + src.len);

    for (size_t i = 0; i < src.len; i++) {
        bool match = src.data[i] == target;
        out_sel->data[out_sel->size] = (int32_t)i;
        out_sel->size += match;
    }
    return true;
}

bool gather_i32_decode(slice_i32_t keys_src, const string_dict_t* dict, const selection_vector_t* sel, const char*** out_strings) {
    if (!keys_src.data || !sel || !out_strings || !dict)
        return false;

    *out_strings = malloc(sel->size * sizeof(char*));
    if (!*out_strings) {
        fprintf(stderr, "[FATAL ERROR] failed to allocate string array.\n");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < sel->size; i++) {
        int32_t k = slice_i32_at(keys_src, (size_t)sel->data[i]);
        (*out_strings)[i] = dict->strings[k];
    }

    return true;
}
