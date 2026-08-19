#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "simpledb/schema_registry.h"

#define GRANULE_SIZE 8192
#define MAX_INDEXES_PER_COLUMN 8

typedef struct index_writer_t index_writer_t;

struct index_writer_t {
    // Function pointer to feed data into the index
    void (*append_fn)(index_writer_t* self, const void* data, size_t num_rows, uint64_t current_row_offset);

    // Function pointer to finalize and close the index files
    void (*close_fn)(index_writer_t* self);

    // Opaque pointer to the specific index's internal state
    void* state;
};

// State for flushing an in-memory column to disk
typedef struct {
    const char* column_name;
    column_type_t type;

    FILE* bin_fp;            // Open file pointer for writing the raw data

    uint64_t rows_written;   // Running total of rows written

    index_writer_t* indexes[MAX_INDEXES_PER_COLUMN];
    size_t num_indexes;
} column_writer_t;

// Initialize a writer for a specific column in a target directory
column_writer_t* column_writer_create(const char* part_dir, const char* col_name, column_type_t type);

// Write a chunk of data to disk, generating marks at 8192-row boundaries
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows);

// Close files and free resources
void column_writer_close(column_writer_t* writer);

// New API function to inject an index from the outside
void column_writer_attach_index(column_writer_t* writer, struct index_writer_t* index);


// ==========================================
// COLUMN READER (READ-PATH)
// ==========================================

#include "simpledb/mmap_file.h"
#include "simpledb/slice.h"
#include "simpledb/bitmap.h"

typedef struct {
    mmap_file_t* mmap;          // The memory-mapped .bin file
    column_type_t type;         // I32, F32, etc.
    const bitmap_t* mask;       // The mask from the Sparse Index!
    size_t total_granules;      // Total granules in the file
    size_t current_granule_idx; // Iterator state
} column_reader_t;

// 1. Open a column for reading. 
// If mask is NULL, it assumes a full table scan (reads everything).
column_reader_t* column_reader_create(const char* part_dir, const char* col_name, column_type_t type, const bitmap_t* mask);

// 2. Fetch the next matching Granule. 
// Returns a slice_i32_t. If len == 0, we have reached the end of the file.
slice_i32_t column_reader_next_granule_i32(column_reader_t* reader);

// 3. Clean up
void column_reader_free(column_reader_t* reader);
