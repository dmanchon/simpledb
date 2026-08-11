#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *bits;
    size_t size;     // Number of tracked bits (rows)
    size_t capacity; // Allocated capacity in bytes
} null_bitmap_t;

null_bitmap_t *null_bitmap_create(size_t initial_capacity_bytes);
void null_bitmap_free(null_bitmap_t *bitmap);

// Set the bit at `index` to 1 (meaning the value IS NULL)
void null_bitmap_set_null(null_bitmap_t *bitmap, size_t index);

// Set the bit at `index` to 0 (meaning the value is NOT NULL)
void null_bitmap_clear_null(null_bitmap_t *bitmap, size_t index);

// Check if the value at `index` is NULL
bool null_bitmap_is_null(const null_bitmap_t *bitmap, size_t index);
