#include "simpledb/part_merger.h"
#include "simpledb/table.h"
#include <stddef.h>
#include <stdlib.h>


typedef struct part_item {
    table_part_t *part;
    size_t part_id;
} part_item;

static int part_comparator(const void* a, const void* b) {
    return ((part_item*)a)->part->row_count - ((part_item*)b)->part->row_count;
}

part_merger_t *part_merger_create(table_t *table, merge_config_t config) {
    part_merger_t *merger = malloc(sizeof(*merger));

    merger->config = config;
    merger->table = table;

    size_t num_parts;
    table_part_t **parts = table_get_parts(table, &num_parts);

    // keep the original id (index)
    part_item *items = malloc(sizeof(part_item) * num_parts);
    for (size_t i = 0; i < merger->num_parts; i++) {
        items[i] = (part_item){.part=parts[i], i};
    }

    // sort in place by num_rows
    qsort(items, num_parts, sizeof(part_item*), part_comparator);

    merger->part_ids_to_merge = malloc(sizeof(size_t) * merger->num_parts);
    for (size_t i = 0; i < merger->num_parts; i++) {
        merger->part_ids_to_merge[i] = i;
    }

    return merger;
}
