#include "simpledb/bitmap.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>


#include <stdio.h>

bitmap_t *bitmap_create(size_t initial_capacity_bytes) {
    bitmap_t *bitmap = malloc(sizeof(bitmap_t));
    if(!bitmap) {
        fprintf(stderr, "[FATAL ERROR] bitmap_create failed to allocate bitmap_t struct\n");
        exit(EXIT_FAILURE);
    }
    bitmap->size = 0;
    bitmap->capacity = initial_capacity_bytes;

    bitmap->bits = (uint8_t *)calloc(initial_capacity_bytes, sizeof(uint8_t));
    if(!bitmap->bits) {
        fprintf(stderr, "[FATAL ERROR] bitmap_create failed to allocate %zu bytes for bit array\n", initial_capacity_bytes);
        exit(EXIT_FAILURE);
    }
    return bitmap;
}

void bitmap_free(bitmap_t *bitmap) {
    if(!bitmap) return;

    free(bitmap->bits);
    free(bitmap);
}

void bitmap_set(bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        fprintf(stderr, "[FATAL ERROR] null_bitmap index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    bitmap->bits[slot] |= 1<<(index%8);
}

void bitmap_clear(bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        fprintf(stderr, "[FATAL ERROR] null_bitmap index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    bitmap->bits[slot] &= ~(1<<(index%8));
}

bool bitmap_is_set(const bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        return false;
    }
    return (bitmap->bits[slot] & (1<<(index%8))) > 0;
}
