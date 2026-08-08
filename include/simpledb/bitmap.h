#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *bits;
    size_t size;     // Number of tracked bits
    size_t capacity; // Allocated capacity in bytes
} bitmap_t;

bitmap_t *bitmap_create(size_t initial_capacity_bytes);
void bitmap_free(bitmap_t *bitmap);

// Set the bit at `index` to 1
void bitmap_set(bitmap_t *bitmap, size_t index);

// Set the bit at `index` to 0
void bitmap_clear(bitmap_t *bitmap, size_t index);

// Check if the bit at `index` is 1
bool bitmap_is_set(const bitmap_t *bitmap, size_t index);
