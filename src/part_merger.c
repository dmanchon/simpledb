#include "simpledb/part_merger.h"
#include "simpledb/min_heap.h"
#include "simpledb/slice.h"
#include "simpledb/storage.h"
#include "simpledb/table.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct part_item {
    table_part_t *part;
    size_t part_id;
} part_item;

typedef struct {
    size_t part_id;
    column_reader_t **readers;

    slice_i32_t *current_granules;
    size_t row_idx;

    int32_t current_key;
    int32_t *current_values;
} part_stream_t;

static int part_comparator(const void *a, const void *b) {
    size_t count_a = ((part_item *)a)->part->row_count;
    size_t count_b = ((part_item *)b)->part->row_count;

    if (count_a < count_b)
        return -1;
    if (count_a > count_b)
        return 1;
    return 0;
}

part_merger_t *part_merger_create(table_t *table, merge_config_t config) {
    part_merger_t *merger = malloc(sizeof(*merger));
    merger->config = config;
    merger->table = table;
    merger->num_parts = 0; // Default to 0 parts (no merge needed)
    merger->part_ids_to_merge = NULL;

    pthread_rwlock_rdlock(&table->catalog_lock);
    size_t current_parts = table->num_parts;
    pthread_rwlock_unlock(&table->catalog_lock);

    // shortcircuit
    if (!config.force_merge_all && current_parts < config.max_parts_to_merge) {
        return merger;
    }
    size_t num_parts;
    table_part_t **parts = table_get_parts(table, &num_parts);

    merger->config = config;
    merger->table = table;

    size_t merge_count =
        (num_parts < config.max_parts_to_merge) ? num_parts : config.max_parts_to_merge;
    merger->num_parts = merge_count;

    // keep the original id (index)
    part_item *items = malloc(sizeof(part_item) * num_parts);
    for (size_t i = 0; i < num_parts; i++) {
        items[i] = (part_item){.part = parts[i], .part_id = i};
    }

    // sort in place by num_rows
    qsort(items, num_parts, sizeof(part_item), part_comparator);

    merger->part_ids_to_merge = malloc(sizeof(size_t) * num_parts);
    for (size_t i = 0; i < merge_count; i++) {
        merger->part_ids_to_merge[i] = items[i].part_id;
    }

    if (!config.force_merge_all && num_parts < config.max_parts_to_merge) {
        size_t total_rows = 0;
        for (size_t i = 0; i < merger->num_parts; i++) {
            size_t id = merger->part_ids_to_merge[i];
            total_rows += parts[id]->row_count;
        }

        if (total_rows < config.min_rows_to_merge) {
            merger->num_parts = 0; // Abort merge, we don't have enough volume yet
        }
    }

    table_release_parts(parts, num_parts);
    free(items);

    return merger;
}

static void stream_fetch_next_and_push(part_stream_t *stream, min_heap_t *heap, size_t num_cols) {
    if (stream->row_idx >= stream->current_granules[0].len) {
        for (size_t j = 0; j < num_cols; j++) {
            stream->current_granules[j] = column_reader_next_granule_i32(stream->readers[j]);
        }
        stream->row_idx = 0; // Reset index for the newly fetched chunk
    }

    if (stream->current_granules[0].len > 0) {
        for (size_t j = 0; j < num_cols; j++) {
            stream->current_values[j] = stream->current_granules[j].data[stream->row_idx];
        }
        stream->current_key = stream->current_values[0];

        min_heap_push(heap, stream->current_key, stream);
    }
}

size_t part_merger_execute(part_merger_t *merger) {

    if (merger->num_parts == 0) {
        return 0; // Abort instantly!
    }

    part_stream_t *streams = malloc(sizeof(part_stream_t) * merger->num_parts);
    for (size_t i = 0; i < merger->num_parts; i++) {
        streams[i].part_id = merger->part_ids_to_merge[i];
        streams[i].readers = malloc(sizeof(column_reader_t *) * merger->table->schema->count);
        streams[i].current_granules = calloc(sizeof(slice_i32_t), merger->table->schema->count);
        streams[i].current_values = malloc(sizeof(int32_t) * merger->table->schema->count);
        streams[i].row_idx = 0;

        for (size_t j = 0; j < merger->table->schema->count; j++) {
            column_reader_t *reader =
                column_reader_create(merger->table->parts[streams[i].part_id]->part_path,
                                     merger->table->schema->columns[j].name,
                                     merger->table->schema->columns[j].type, NULL);
            streams[i].readers[j] = reader;
        }
    }

    min_heap_t *heap = min_heap_create(merger->num_parts);

    for (size_t i = 0; i < merger->num_parts; i++) {
        stream_fetch_next_and_push(&streams[i], heap, merger->table->schema->count);
    }

    uint64_t new_min_block = UINT64_MAX;
    uint64_t new_max_block = 0;
    uint32_t max_level = 0;
    for (size_t i = 0; i < merger->num_parts; i++) {
        table_part_t *p = merger->table->parts[merger->part_ids_to_merge[i]];
        if (p->min_block < new_min_block) new_min_block = p->min_block;
        if (p->max_block > new_max_block) new_max_block = p->max_block;
        if (p->level > max_level) max_level = p->level;
    }
    if (new_min_block == UINT64_MAX) new_min_block = 0;
    uint32_t new_level = max_level + 1;

    char new_part_dir[256];
    table_part_format_name(new_part_dir, sizeof(new_part_dir), merger->table->name, new_min_block, new_max_block, new_level);
    mkdir(merger->table->name, 0777);
    mkdir(new_part_dir, 0777);
    column_writer_t **writers = malloc(sizeof(column_writer_t *) * merger->table->schema->count);

    for (size_t j = 0; j < merger->table->schema->count; j++) {
        writers[j] = column_writer_create(new_part_dir, merger->table->schema->columns[j].name,
                                          merger->table->schema->columns[j].type);
    }
    heap_node_t min_node;
    while (min_heap_pop(heap, &min_node)) {
        part_stream_t *stream = (part_stream_t *)min_node.payload;
        for (size_t j = 0; j < merger->table->schema->count; j++) {
            column_writer_append(writers[j], (void *)&stream->current_values[j], 1);
        }
        stream->row_idx++;
        stream_fetch_next_and_push(stream, heap, merger->table->schema->count);
    }

    // cleanup
    for (size_t j = 0; j < merger->table->schema->count; j++) {
        column_writer_close(writers[j]);
    }
    free(writers);

    min_heap_destroy(heap);
    for (size_t i = 0; i < merger->num_parts; i++) {
        free(streams[i].current_granules);
        free(streams[i].current_values);
        for (size_t j = 0; j < merger->table->schema->count; j++) {
            column_reader_free(streams[i].readers[j]);
        }
        free(streams[i].readers);
    }
    free(streams);

    // so later we can deal with this in the GC
    merger->zombies = malloc(sizeof(table_part_t *) * merger->num_parts);
    for (size_t i = 0; i < merger->num_parts; i++) {
        merger->zombies[i] = merger->table->parts[merger->part_ids_to_merge[i]];
    }

    // atomic switch part
    table_part_t *new_part = malloc(sizeof(table_part_t));
    strcpy(new_part->part_path, new_part_dir);
    new_part->min_block = new_min_block;
    new_part->max_block = new_max_block;
    new_part->level = new_level;
    atomic_init(&new_part->refcount, 0);
    // The new row count is just the sum of all the old parts you merged!
    new_part->row_count = 0;
    for (size_t i = 0; i < merger->num_parts; i++) {
        new_part->row_count += merger->table->parts[merger->part_ids_to_merge[i]]->row_count;
    }
    pthread_rwlock_wrlock(&merger->table->catalog_lock);
    size_t new_count = merger->table->num_parts - merger->num_parts + 1;

    table_part_t **parts = malloc(sizeof(table_part_t *) * new_count);

    size_t current_index = 0;
    for (size_t i = 0; i < merger->table->num_parts; i++) {
        bool is_merged = false;

        for (size_t j = 0; j < merger->num_parts; j++) {
            if (i == merger->part_ids_to_merge[j]) {
                is_merged = true;
            }
        }
        if (is_merged)
            continue;
        parts[current_index++] = merger->table->parts[i];
    }
    parts[current_index++] = new_part;

    free(merger->table->parts);
    merger->table->parts = parts;
    merger->table->num_parts = new_count;
    pthread_rwlock_unlock(&merger->table->catalog_lock);

    return 0;
}

void part_merger_destroy(part_merger_t *merger) {
    if (!merger) return;
    free(merger->part_ids_to_merge);
    free(merger->zombies);
    free(merger);
}

void *table_background_compactor_thread(void *arg) {
    table_t *table = (table_t *)arg;

    merge_config_t config = {
        .max_parts_to_merge = 10, .min_rows_to_merge = 50000, .force_merge_all = false};

    table_part_t **daemon_zombies = NULL;
    size_t daemon_zombie_count = 0;

    while (1) {
        sleep(5);

        part_merger_t *merger = part_merger_create(table, config);
        part_merger_execute(merger);
        daemon_zombies = realloc(daemon_zombies, sizeof(table_part_t *) *
                                                     (daemon_zombie_count + merger->num_parts));
        for (size_t i = 0; i < merger->num_parts; i++) {
            daemon_zombies[daemon_zombie_count++] = merger->zombies[i];
        }

        // GC
        for (size_t i = 0; i < daemon_zombie_count; i++) {
            // non-blocking, we just wait to next iteration if a query holds the part
            if (atomic_load(&daemon_zombies[i]->refcount) == 0) {
                free(daemon_zombies[i]);

                daemon_zombies[i] = daemon_zombies[daemon_zombie_count - 1];
                daemon_zombie_count--;
                i--;
            }
        }

        part_merger_destroy(merger);
    }
    return NULL;
}
