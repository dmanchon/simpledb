#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Represents a contiguous range of granules to scan
typedef struct {
    uint32_t start_granule; // Inclusive
    uint32_t end_granule;   // Exclusive
} granule_range_t;

// The in-memory representation of the primary.idx file
typedef struct {
    int32_t* marks;     // Array of primary key values, one per granule
    size_t num_marks;   // Number of granules
    int32_t min_val;    // Absolute min for the entire part
    int32_t max_val;    // Absolute max for the entire part
} sparse_index_t;

// Load a sparse index from disk
sparse_index_t* sparse_index_load(const char* idx_file_path);

// Free the index
void sparse_index_free(sparse_index_t* idx);

// 2-Tier Pruning logic: Find which granules might contain the target_val
// Tier 1: Check [min_val, max_val] to see if the part should be skipped entirely
// Tier 2: Binary search the marks to find the [start, end) granule range
// Returns true if a valid range is found, false if the target_val is definitely not in this part.
bool sparse_index_search(const sparse_index_t* idx, int32_t target_val, granule_range_t* out_range);
