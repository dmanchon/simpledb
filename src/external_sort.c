#include "simpledb/external_sort.h"
#include <_string.h>
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
    sorter->heap = (min_heap_t){0};
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
    free(sorter->heap.data);

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

static void heap_sift_up(min_heap_t *heap, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        if (heap->data[idx].record.key_value < heap->data[parent].record.key_value) {
            heap_node_t tmp = heap->data[idx];
            heap->data[idx] = heap->data[parent];
            heap->data[parent] = tmp;
            idx = parent;
        } else {
            break;
        }
    }
}

static void heap_sift_down(min_heap_t *heap, size_t idx) {
    size_t size = heap->size;
    // As long as idx has at least a left child:
    while (2 * idx + 1 < size) {
        size_t left = 2 * idx + 1;
        size_t right = 2 * idx + 2;
        size_t smallest = idx;

        // Check if Left child is smaller
        if (left < size &&
            heap->data[left].record.key_value < heap->data[smallest].record.key_value) {
            smallest = left;
        }
        // Check if Right child is even smaller
        if (right < size &&
            heap->data[right].record.key_value < heap->data[smallest].record.key_value) {
            smallest = right;
        }

        // If a child is smaller than parent, swap and continue sinking
        if (smallest != idx) {
            heap_node_t tmp = heap->data[idx];
            heap->data[idx] = heap->data[smallest];
            heap->data[smallest] = tmp;
            idx = smallest; // Follow the sinking node
        } else {
            // Both children are >= current node. It's in the right spot!
            break;
        }
    }
}

int external_sorter_finish(external_sorter_t *sorter) {
    if (!sorter || sorter->finished_ingestion)
        return -1;

    // process last chunk
    if (sorter->num_runs > 0) {
        // store the current run in a file
        spill_run(sorter);
        // setup the heap
        sorter->heap.data = malloc(sizeof(heap_node_t) * sorter->num_runs);
        if (!sorter->heap.data)
            return -1;
        sorter->heap.capacity = sorter->num_runs;
        sorter->heap.size = 0;

        // prepare the files to be iterated
        sorter->run_files = malloc(sizeof(FILE *) * sorter->num_runs);
        if (!sorter->run_files)
            return -1;

        for (size_t i = 0; i < sorter->num_runs; i++) {
            sorter->run_files[i] = fopen(sorter->run_paths[i], "rb");
            if (!sorter->run_files[i])
                return -1;

            // push first element into the heap
            sort_record_t record;
            if (fread(&record, sizeof(sort_record_t), 1, sorter->run_files[i]) != 1)
                return -1;
            heap_node_t node = {.record = record, .run_idx = i};
            sorter->heap.data[sorter->heap.size++] = node;
            heap_sift_up(&sorter->heap, sorter->heap.size - 1);
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
    return 0;
}
