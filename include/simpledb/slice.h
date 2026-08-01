#pragma once
#include "simpledb/allocator.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    const int32_t* data;
    size_t len;
} slice_i32_t;

typedef struct {
    const float* data;
    size_t len;
} slice_f32_t;

static inline slice_i32_t  slice_i32_create(const int32_t* buff, size_t len) {
    return (slice_i32_t) {.data = buff, .len=len};

}
static inline slice_f32_t slice_f32_create(const float* buff, size_t len) {
    return (slice_f32_t) {.data = buff, .len=len};
}

static inline slice_i32_t buffer_i32_as_slice(const buffer_i32_t* buff) {
    if (!buff || !buff->data) return (slice_i32_t){ .data = NULL, .len = 0 };
    return (slice_i32_t) {
        .data = (const int32_t*) buff->data,
        .len = buff->size
    };
}

static inline slice_f32_t buffer_f32_as_slice(const buffer_i32_t* buff) {
    if (!buff || !buff->data) return (slice_f32_t){ .data = NULL, .len = 0 };
    return (slice_f32_t){
        .data = (const float*)buff->data,
        .len = buff->size
    };
}

static inline slice_i32_t slice_i32_sub(slice_i32_t s, size_t offset, size_t count) {
    return (slice_i32_t) {.data=(const int32_t*)s.data+offset, .len=count};
}


static inline slice_f32_t slice_f32_sub(slice_f32_t s, size_t offset, size_t count) {
    return (slice_f32_t) {.data=(const float*)s.data+offset, .len=count};
}

static inline int32_t slice_i32_at(slice_i32_t s, size_t idx) {
    return s.data[idx];
}

static inline float slice_f32_at(slice_f32_t s, size_t idx) {
    return s.data[idx];
}

#define slice_of(PTR_OR_BUF, LEN) _Generic((PTR_OR_BUF), \
    int32_t*:       slice_i32_create((const int32_t*)(PTR_OR_BUF), (LEN)), \
    const int32_t*: slice_i32_create((const int32_t*)(PTR_OR_BUF), (LEN)), \
    float*:         slice_f32_create((const float*)(PTR_OR_BUF), (LEN)), \
    const float*:   slice_f32_create((const float*)(PTR_OR_BUF), (LEN)), \
    buffer_i32_t*:  buffer_i32_as_slice((const buffer_i32_t*)(PTR_OR_BUF)) \
)
