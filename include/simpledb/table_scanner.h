#pragma once

#include "simpledb/table.h"
#include "simpledb/storage.h"
#include "simpledb/slice.h"
#include <stdbool.h>

#include "simpledb/filter.h"

// A Block represents a chunk of data yielded by the scanner (e.g., one Granule).
// It contains parallel arrays (slices) for each requested column.
typedef struct {
    size_t num_rows;
    size_t num_columns;
    // For simplicity in this iteration, we assume all requested columns are I32.
    // In a full implementation, this would be an array of polymorphic column views.
    slice_i32_t* columns; 
    
    // Optional zero-copy selection vector for Late Materialization filtering
    selection_vector_t* sel_vector; 
} block_t;

typedef struct table_scanner_t {
    table_t* table;
    const char** column_names;
    size_t num_columns;
    
    // Iterator state for Disk Parts
    size_t current_part_idx;
    column_reader_t** current_readers;
    bitmap_t* current_mask;
    
    // State flags
    bool finished_disk_parts;
    bool finished_memory_buffer;
} table_scanner_t;

// 1. Create a scanner for specific columns of a table
table_scanner_t* table_scanner_create(table_t* table, const char** column_names, size_t num_columns);

// 2. Yield the next block of data. 
// If block.num_rows == 0, End Of File (EOF) is reached.
block_t table_scanner_next(table_scanner_t* scanner);

// 3. Clean up the scanner
void table_scanner_destroy(table_scanner_t* scanner);
