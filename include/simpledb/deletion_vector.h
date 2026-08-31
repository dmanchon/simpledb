#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DV_INITIAL_CAPACITY 16

// Roaring Container Types
typedef enum {
    CONTAINER_ARRAY,  // Stored as an array of uint16_t (for < 4096 deletes)
    CONTAINER_BITMAP, // Stored as a flat 8KB bitmap (for >= 4096 deletes)
    CONTAINER_RUN     // Stored as start/length pairs (for contiguous deletes)
} container_type_t;

typedef struct {
    container_type_t type;
    uint16_t cardinality;
    uint32_t capacity;
    void *data; // Pointer to the underlying array, bitmap, or run data
} roaring_container_t;

// The Deletion Vector abstraction
typedef struct {
    uint32_t num_rows;
    uint32_t deleted_count;

    // Map of high-16 bit keys to their respective containers
    uint16_t *keys;
    roaring_container_t **containers;

    uint32_t num_containers;
    uint32_t capacity;
} deletion_vector_t;

// --- Lifecycle ---
deletion_vector_t* deletion_vector_create(uint32_t initial_capacity);
void deletion_vector_free(deletion_vector_t *dv);

// --- Mutations ---
void deletion_vector_add(deletion_vector_t *dv, uint32_t row_id);

// --- Queries ---
// Returns the total number of deleted rows (O(1) lookup)
uint64_t deletion_vector_get_cardinality(const deletion_vector_t *dv);

bool deletion_vector_is_deleted(const deletion_vector_t *dv, uint32_t row_id);

// Returns a flat 64-bit bitmask of deleted rows starting at `start_row`.
// Used by SIMD execution pipelines (AND NOT) to filter vector blocks!
uint64_t deletion_vector_get_simd_mask(const deletion_vector_t *dv, uint32_t start_row);
