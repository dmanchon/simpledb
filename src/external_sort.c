#include "simpledb/external_sort.h"
#include "simpledb/min_heap.h"
#include <_string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

external_sorter_t *external_sorter_create(const char *temp_dir, size_t mem_limit_records) {
    external_sorter_t *sorter = malloc(sizeof(*sorter));

    if (!sorter) {
        fprintf(stderr,
                "[FATAL ERROR] external_sorter_create failed to allocate external_sorter_t\n");
        exit(EXIT_FAILURE);
    }

    sorter->mem_limit_records = mem_limit_records;
    sorter->finished_ingestion = false;
    sorter->in_mem_buf = malloc(mem_limit_records * sizeof(*sorter->in_mem_buf));
    if (!sorter->in_mem_buf) {
        free(sorter);
        fprintf(stderr, "[FATAL ERROR] in_mem_buf failed to allocate in_mem_buf\n");
        exit(EXIT_FAILURE);
    }
    sorter->in_mem_count = 0;
    sorter->num_runs = 0;
    sorter->run_paths = NULL;
    sorter->run_files = NULL;
    sorter->run_paths_capacity = 0;
    sorter->in_mem_iter_idx = 0;
    sorter->run_current_records = NULL;
    sorter->heap = NULL;
    snprintf(sorter->temp_dir, sizeof(sorter->temp_dir), "%s", temp_dir);

    return sorter;
}

void external_sorter_destroy(external_sorter_t *sorter) {
    if (!sorter)
        return;

    if (sorter->run_files) {
        for (size_t i = 0; i < sorter->num_runs; i++) {
            if (sorter->run_files[i]) {
                fclose(sorter->run_files[i]);
            }
        }
        free(sorter->run_files);
    }

    for (size_t i = 0; i < sorter->num_runs; i++) {
        char *path = sorter->run_paths[i];
        remove(path);
        free(sorter->run_paths[i]);
    }
    free(sorter->run_paths);
    free(sorter->run_current_records);

    if (sorter->heap) {
        min_heap_destroy(sorter->heap);
    }
    free(sorter->in_mem_buf);
    free(sorter);
}

static int sort_fn(const void *a, const void *b) {
    const sort_record_t *ra = (const sort_record_t *)a;
    const sort_record_t *rb = (const sort_record_t *)b;
    if (ra->key_value < rb->key_value)
        return -1;
    if (ra->key_value > rb->key_value)
        return 1;
    return 0;
}

static int spill_run(external_sorter_t *sorter) {
    if (sorter->in_mem_count == 0)
        return 0;
    // sort it with qsort
    qsort(sorter->in_mem_buf, sorter->in_mem_count, sizeof(sort_record_t), sort_fn);
    // serialize to file
    if (sorter->run_paths_capacity <= sorter->num_runs) {
        // resize
        size_t new_cap = (sorter->run_paths_capacity == 0) ? 8 : sorter->run_paths_capacity * 2;
        sorter->run_paths = realloc(sorter->run_paths, sizeof(char *) * new_cap);
        if (!sorter->run_paths) {
            return -1;
        }
        sorter->run_paths_capacity = new_cap;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/run_%04zu.run", sorter->temp_dir, sorter->num_runs);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    size_t written = fwrite(sorter->in_mem_buf, sizeof(sort_record_t), sorter->in_mem_count, fp);
    fclose(fp);
    if (written != sorter->in_mem_count) {
        remove(path); // Delete the corrupt, partial file
        return -1;    // Propagate failure to caller
    }
    sorter->run_paths[sorter->num_runs++] = strdup(path);

    // reset
    sorter->in_mem_count = 0;
    return 0;
}

int external_sorter_push(external_sorter_t *sorter, const sort_record_t *records, size_t count) {
    if (!sorter || (!records && count > 0) || sorter->finished_ingestion) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        sorter->in_mem_buf[sorter->in_mem_count++] = records[i];
        if (sorter->in_mem_count == sorter->mem_limit_records) {
            if (spill_run(sorter) != 0)
                return -1;
        }
    }

    return 0;
}

int external_sorter_finish(external_sorter_t *sorter) {
    if (!sorter || sorter->finished_ingestion)
        return -1;

    // process last chunk
    if (sorter->num_runs > 0) {
        // store the current run in a file
        spill_run(sorter);
        // setup the heap
        sorter->heap = min_heap_create(sorter->num_runs);
        if (!sorter->heap)
            return -1;

        // prepare the files to be iterated
        sorter->run_files = malloc(sizeof(FILE *) * sorter->num_runs);
        if (!sorter->run_files)
            return -1;

        sorter->run_current_records = malloc(sizeof(sort_record_t) * sorter->num_runs);
        if (!sorter->run_current_records)
            return -1;

        for (size_t i = 0; i < sorter->num_runs; i++) {
            sorter->run_files[i] = fopen(sorter->run_paths[i], "rb");
            if (!sorter->run_files[i])
                return -1;

            // push first element into the heap
            sort_record_t record;
            if (fread(&record, sizeof(sort_record_t), 1, sorter->run_files[i]) != 1)
                return -1;

            sorter->run_current_records[i] = record;
            min_heap_push(sorter->heap, record.key_value, (void *)(uintptr_t)i);
        }

    } else {
        // everything fits in memory, so sort it!
        qsort(sorter->in_mem_buf, sorter->in_mem_count, sizeof(sort_record_t), sort_fn);
    }

    sorter->in_mem_iter_idx = 0;
    sorter->finished_ingestion = true;
    return 0;
}

// read path
bool external_sorter_next(external_sorter_t *sorter, sort_record_t *out_record) {
    if (!sorter || !out_record || !sorter->finished_ingestion) {
        return false;
    }

    if (sorter->num_runs > 0) {
        heap_node_t top;
        // If heap is empty, all runs on disk reached EOF -> stream is done!
        if (!min_heap_peek(sorter->heap, &top)) {
            return false;
        }

        size_t run_idx = (size_t)(uintptr_t)top.payload;
        *out_record = sorter->run_current_records[run_idx];

        sort_record_t record;
        if (fread(&record, sizeof(sort_record_t), 1, sorter->run_files[run_idx]) == 1) {
            sorter->run_current_records[run_idx] = record;
            min_heap_replace_top(sorter->heap, record.key_value, (void *)(uintptr_t)run_idx);
        } else {
            min_heap_pop(sorter->heap, &top);
        }
        return true;
    } else {
        if (sorter->in_mem_iter_idx < sorter->in_mem_count) {
            *out_record = sorter->in_mem_buf[sorter->in_mem_iter_idx++];
            return true;
        }
        return false;
    }
}
