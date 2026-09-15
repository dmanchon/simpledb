#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// A sort record / tuple holding a 32-bit integer sort key value and a 32-bit row_id
typedef struct {
    union {
        int32_t key;       // Traditional name: sort key
        int32_t key_value; // Explicit name: the actual value of the sort key (e.g. score = 92)
    };
    int32_t row_id;        // Late materialization pointer: tuple index in the table
} sort_record_t;

// A heap node used during K-way merge
typedef struct {
    sort_record_t record;
    size_t run_idx;
} heap_node_t;

// Min-heap structure for K-way merge
typedef struct {
    heap_node_t *data;
    size_t size;
    size_t capacity;
} min_heap_t;

// External sorter state
typedef struct external_sorter_t {
    char temp_dir[256];
    size_t mem_limit_records;  // Max records allowed in RAM buffer before spilling
    sort_record_t *in_mem_buf; // RAM buffer for run generation
    size_t in_mem_count;       // Current count in RAM buffer

    char **run_paths;          // Array of filepaths for temporary runs
    FILE **run_files;          // Open FILE pointers during merge phase
    size_t num_runs;           // Total number of spilled runs
    size_t run_paths_capacity;

    bool finished_ingestion;   // True after external_sorter_finish() is called
    min_heap_t heap;           // Priority queue for K-way merge
    size_t in_mem_iter_idx;    // If all records fit in RAM (0 runs spilled), iterator offset
} external_sorter_t;

// Create and destroy sorter
external_sorter_t *external_sorter_create(const char *temp_dir, size_t mem_limit_records);
void external_sorter_destroy(external_sorter_t *sorter);

// Push a batch of records into the sorter (spills to disk if RAM buffer fills up)
int external_sorter_push(external_sorter_t *sorter, const sort_record_t *records, size_t count);

// Seal ingestion and prepare for streaming sorted output
int external_sorter_finish(external_sorter_t *sorter);

// Pull next lowest record from the K-way merge heap (returns true if record found, false on EOF)
bool external_sorter_next(external_sorter_t *sorter, sort_record_t *out_record);
