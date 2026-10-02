#pragma once

#include "simpledb/table.h"
#include <stdbool.h>
#include <stddef.h>

// Configuration for the compaction policy
typedef struct {
    size_t max_parts_to_merge;
    size_t min_rows_to_merge;
    bool force_merge_all; // E.g., for an OPTIMIZE TABLE command
} merge_config_t;

// The state of a running compaction job
typedef struct {
    table_t* table;
    merge_config_t config;
    
    // Internal state tracking which parts are currently being merged
    size_t* part_ids_to_merge;
    size_t num_parts;
} part_merger_t;

// Initialize a part merger job for a table
part_merger_t* part_merger_create(table_t* table, merge_config_t config);

// Execute the merge on the main thread (blocking). 
// Returns the ID of the newly created consolidated part.
// Returns (size_t)-1 if no merge was necessary or an error occurred.
size_t part_merger_execute(part_merger_t* merger);

// Free the merger job resources
void part_merger_destroy(part_merger_t* merger);

// ---------------------------------------------------------
// Background Thread API
// ---------------------------------------------------------

// A high-level helper that evaluates the compaction policy in a loop.
// If the table needs compaction, it creates a merger and runs it.
// This is called by the background thread.
void* table_background_compactor_thread(void* arg);

// Starts the compactor in a background pthread
void table_start_background_compactor(table_t* table);
