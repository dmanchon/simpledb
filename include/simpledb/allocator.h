#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SIMD_ALIGNMENT 64

typedef struct buffer_i32_t {
    size_t size;
    size_t capacity;
    int32_t *data;
} buffer_i32_t;

// Cold path functions (Declared in header, implemented in src/allocator.c)
void* aligned_malloc(size_t bytes);
void  aligned_free(void* ptr);

buffer_i32_t* buffer_i32_create(size_t initial_capacity);
void          buffer_i32_free(buffer_i32_t* buf);
void          buffer_i32_grow(buffer_i32_t* buf);

// Hot path function (Kept static inline in header for zero call overhead in loops)
static inline bool buffer_i32_push(buffer_i32_t* buf, int32_t val) {
    if (!buf) return false;

    if (buf->size >= buf->capacity) {
        buffer_i32_grow(buf);
    }

    buf->data[buf->size++] = val;
    return true;
}
