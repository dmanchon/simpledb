#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "simpledb/schema_registry.h"

#define GRANULE_SIZE 8192

// Represents a single entry in the .mrk file
typedef struct {
    uint64_t offset_in_bin;  // The byte offset in the .bin file where this granule starts
    uint64_t num_rows;       // Usually 8192, but the final granule might be smaller
} mark_entry_t;

// State for flushing an in-memory column to disk
typedef struct {
    const char* column_name;
    column_type_t type;

    FILE* bin_fp;            // Open file pointer for writing the raw data
    FILE* mrk_fp;            // Open file pointer for writing the marks

    uint64_t current_offset; // Running total of bytes written to bin_fp
    uint64_t rows_written;   // Running total of rows written
} column_writer_t;

// Initialize a writer for a specific column in a target directory
column_writer_t* column_writer_create(const char* part_dir, const char* col_name, column_type_t type);

// Write a chunk of data to disk, generating marks at 8192-row boundaries
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows);

// Close files and free resources
void column_writer_close(column_writer_t* writer);
