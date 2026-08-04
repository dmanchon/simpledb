#pragma once

#include "simpledb/slice.h"
#include <stddef.h>
#include <stdlib.h>

typedef struct {
    const void *data;
    size_t      size;
    int         fd;
} mmap_file_t;

mmap_file_t *mmap_file_open(const char *filepath);
void         mmap_file_close(mmap_file_t *file);

static inline slice_i32_t mmap_file_as_slice_i32(const mmap_file_t *file) {
    if (!file || !file->data) {
        return (slice_i32_t){ .data = NULL, .len = 0 };
    }
    return (slice_i32_t){
        .data = (const int32_t *)file->data,
        .len  = file->size / sizeof(int32_t)
    };
}
