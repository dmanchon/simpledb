#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// A record representing an input row for aggregation.
// We assume a simple GROUP BY (1 int32 key) and one SUM/COUNT aggregation (1 int32 value).
typedef struct {
    int32_t group_key;
    int32_t agg_value;
} agg_input_record_t;

// A record representing the aggregated output.
typedef struct {
    int32_t group_key;
    int64_t sum_value;
    int64_t count_value;
} agg_output_record_t;

// Grace Hash Aggregator state
typedef struct grace_hash_agg_t {
    char temp_dir[256];
    size_t num_partitions;
    size_t mem_limit_records;

    // In-memory buffer for incoming records before they are partitioned/spilled
    agg_input_record_t *in_mem_buf;
    size_t in_mem_count;

    // File pointers for each partition's spill file
    FILE **partition_files;
    char **partition_paths;
    
    // Total records spilled to each partition
    size_t *partition_counts;

    bool finished_ingestion;

    // For the output phase: current partition being aggregated in memory
    size_t current_partition_idx;
    
    // In-memory hash table for the current partition
    // (Mock implementation will use a simple array or similar structure)
    agg_output_record_t *current_hash_table;
    size_t hash_table_size;
    size_t hash_table_capacity;
    size_t hash_table_iter_idx;
    
} grace_hash_agg_t;

// Create and destroy the Grace Hash Aggregator
grace_hash_agg_t *grace_hash_agg_create(const char *temp_dir, size_t mem_limit_records, size_t num_partitions);
void grace_hash_agg_destroy(grace_hash_agg_t *agg);

// Push a batch of records into the aggregator. 
// If buffers fill up, records are hashed and spilled to partition files.
int grace_hash_agg_push(grace_hash_agg_t *agg, const agg_input_record_t *records, size_t count);

// Seal ingestion, flush all buffers to disk, and prepare for output phase
int grace_hash_agg_finish(grace_hash_agg_t *agg);

// Pull the next aggregated result. Returns true if a record was found, false on EOF.
bool grace_hash_agg_next(grace_hash_agg_t *agg, agg_output_record_t *out_record);
