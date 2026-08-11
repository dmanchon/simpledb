#include "simpledb/null_bitmap.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>


#include <stdio.h>

null_bitmap_t *null_bitmap_create(size_t initial_capacity_bytes) {
    null_bitmap_t *bitmap = malloc(sizeof(null_bitmap_t));
    if(!bitmap) {
        fprintf(stderr, "[FATAL ERROR] null_bitmap_create failed to allocate null_bitmap_t struct\n");
        exit(EXIT_FAILURE);
    }
    bitmap->size = 0;
    bitmap->capacity = initial_capacity_bytes;

    bitmap->bits = (uint8_t *)calloc(initial_capacity_bytes, sizeof(uint8_t));
    if(!bitmap->bits) {
        fprintf(stderr, "[FATAL ERROR] null_bitmap_create failed to allocate %zu bytes for bit array\n", initial_capacity_bytes);
        exit(EXIT_FAILURE);
    }
    return bitmap;
}

void null_bitmap_free(null_bitmap_t *bitmap) {
    if(!bitmap) return;

    free(bitmap->bits);
    free(bitmap);
}

void null_bitmap_set_null(null_bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        fprintf(stderr, "[FATAL ERROR] null_bitmap index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    bitmap->bits[slot] |= 1<<(index%8);
}

void null_bitmap_clear_null(null_bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        fprintf(stderr, "[FATAL ERROR] null_bitmap index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    bitmap->bits[slot] &= ~(1<<(index%8));
}

bool null_bitmap_is_null(const null_bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        return false;
    }
    return (bitmap->bits[slot] & (1<<(index%8))) > 0;
}
