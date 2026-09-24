#include "simpledb/grace_hash_agg.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>

grace_hash_agg_t *grace_hash_agg_create(const char *temp_dir, size_t mem_limit_records,
                                        size_t num_partitions) {
    grace_hash_agg_t *agg = malloc(sizeof(*agg));

    agg->in_mem_count = 0;
    agg->mem_limit_records = mem_limit_records;
    agg->in_mem_buf = malloc(sizeof(agg_input_record_t) * mem_limit_records);
    if (!agg->in_mem_buf) {
        free(agg);
        return NULL;
    }

    agg->partition_counts = calloc(sizeof(size_t), num_partitions);
    if (!agg->partition_counts) {
        free(agg->in_mem_buf);
        free(agg);
        return NULL;
    }

    agg->partition_files = malloc(sizeof(FILE *) * num_partitions);
    if (!agg->partition_files) {
        free(agg->in_mem_buf);
        free(agg->partition_counts);
        free(agg);
        return NULL;
    }

    agg->partition_paths = malloc(sizeof(char *) * num_partitions);
    if (!agg->partition_paths) {
        free(agg->in_mem_buf);
        free(agg->partition_counts);
        free(agg->partition_files);
        free(agg);
        return NULL;
    }
    char *flat_buffer = calloc(sizeof(char), 256 * num_partitions);
    for (size_t i = 0; i < num_partitions; i++) {
        agg->partition_paths[i] = flat_buffer + (i * 256);
        snprintf(agg->partition_paths[i], 256, "%s/part_%zu.bin", temp_dir, i);
    }

    agg->num_partitions = num_partitions;
    snprintf(agg->temp_dir, sizeof(agg->temp_dir), "%s", temp_dir);

    return agg;
}

void grace_hash_agg_destroy(grace_hash_agg_t *agg) {
    if (!agg)
        return;

    free(agg->partition_counts);
    free(agg->in_mem_buf);

    for (size_t i = 0; i < agg->num_partitions; i++) {
        if (agg->partition_files[i]) {
            fclose(agg->partition_files[i]);
            remove(agg->partition_paths[i]);
        }
    }
    free(agg->partition_files);
    free(agg->partition_paths[0]);
    free(agg->partition_paths);

    free(agg);
}

int grace_hash_agg_push(grace_hash_agg_t *agg, const agg_input_record_t *records, size_t count) {
    for (size_t i = 0; i < count; i++) {
        agg->in_mem_buf[agg->in_mem_count++] = records[i];

        if (agg->in_mem_count == agg->mem_limit_records) {
            for (size_t j = 0; j < agg->mem_limit_records; j++) {
                uint32_t p = agg->in_mem_buf[j].group_key % agg->num_partitions;
                if (agg->partition_files[p] == NULL) {
                    // new file
                    agg->partition_files[p] = fopen(agg->partition_paths[p], "ab");
                    if (!agg->partition_files[p]) {
                        return -1;
                    }
                }
                fwrite(&agg->in_mem_buf[j], sizeof(agg_input_record_t), 1, agg->partition_files[p]);
                agg->partition_counts[p]++;
            }
            agg->in_mem_count = 0;
        }
    }
    return 0;
}

int grace_hash_agg_finish(grace_hash_agg_t *agg) {
    for (size_t j = 0; j < agg->in_mem_count; j++) {
        uint32_t p = agg->in_mem_buf[j].group_key % agg->num_partitions;
        if (agg->partition_files[p] == NULL) {
            // new file
            agg->partition_files[p] = fopen(agg->partition_paths[p], "ab");
            if (!agg->partition_files[p]) {
                return -1;
            }
        }
        fwrite(&agg->in_mem_buf[j], sizeof(agg_input_record_t), 1, agg->partition_files[p]);
        agg->partition_counts[p]++;
    }

    for (size_t p = 0; p < agg->num_partitions; p++) {
        if (agg->partition_files[p] != NULL) {
            fclose(agg->partition_files[p]);
        }
    }
    agg->finished_ingestion = true;

    return 0;
}

bool grace_hash_agg_next(grace_hash_agg_t *agg, agg_output_record_t *out_record) {
    if (!agg || agg->current_partition_idx >= agg->num_partitions) {
        return false;
    }

    while (agg->hash_table_iter_idx >= agg->hash_table_size || !agg->current_hash_table) {
        if (agg->current_partition_idx >= agg->num_partitions)
            return false;
        // reset state
        free(agg->current_hash_table);
        agg->current_hash_table = NULL;
        agg->hash_table_size = 0;
        agg->hash_table_iter_idx = 0; agg->hash_table_capacity = 0;
        // next partition
        agg_input_record_t in;
        agg->partition_files[agg->current_partition_idx] =
            fopen(agg->partition_paths[agg->current_partition_idx], "rb");
        if (agg->partition_files[agg->current_partition_idx] == NULL) { agg->current_partition_idx++; continue; } while (fread(&in, sizeof(in), 1, agg->partition_files[agg->current_partition_idx]) == 1) {
            bool exists = false;
            for (size_t i = 0; i < agg->hash_table_size; i++) {
                if (agg->current_hash_table[i].group_key == in.group_key) {
                    exists = true;
                    agg->current_hash_table[i].count_value++;
                    agg->current_hash_table[i].sum_value += in.agg_value;
                }
            }
            if (!exists) {
                if (agg->hash_table_size >= agg->hash_table_capacity) {
                    // resize
                    agg->hash_table_capacity =
                        (agg->hash_table_capacity > 0) ? agg->hash_table_capacity * 2 : 16;
                    agg->current_hash_table = realloc(
                        agg->current_hash_table, sizeof(*out_record) * agg->hash_table_capacity);
                }

                agg_output_record_t new = {
                    .count_value = 1, .sum_value = in.agg_value, .group_key = in.group_key};
                printf("Added to hash table size=%zu group=%d\n", agg->hash_table_size, new.group_key); agg->current_hash_table[agg->hash_table_size++] = new;
            }
        };
        fclose(agg->partition_files[agg->current_partition_idx]);
        agg->current_partition_idx++;

    }

    *out_record = agg->current_hash_table[agg->hash_table_iter_idx++];
    return true;
}
