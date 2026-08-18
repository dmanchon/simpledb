#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "simpledb/bitmap.h"
#include "simpledb/storage.h"

// The physical header written at the very beginning of every .idx file
typedef struct {
    uint32_t magic;      // e.g., 0x5052494D ('P' 'R' 'I' 'M')
    uint32_t version;    // e.g., 1
    size_t num_marks;    // Number of 32-bit marks that follow this header
    int32_t min_val;     // Absolute minimum value for the entire part
    int32_t max_val;     // Absolute maximum value for the entire part
} sparse_index_file_header_t;

// The in-memory representation of the primary.idx file
typedef struct {
    sparse_index_file_header_t header; // Embedded physical metadata
    int32_t* marks;                    // Array of primary key values, one per granule
} sparse_index_t;

// --- Read-Path ---
// Load a sparse index from disk
sparse_index_t* sparse_index_load(const char* idx_file_path);

// Free the index
void sparse_index_free(sparse_index_t* idx);

// 2-Tier Pruning logic
bool sparse_index_search(const sparse_index_t* idx, int32_t target_val, bitmap_t* out_mask);

// --- Write-Path Factory ---
// Creates the concrete index plugin used by storage.c to write the index
struct index_writer_t* sparse_index_writer_create(const char* part_dir, const char* col_name);
