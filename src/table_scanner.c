#include "simpledb/table_scanner.h"
#include "simpledb/slice.h"
#include "simpledb/storage.h"
#include "simpledb/table.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

table_scanner_t *table_scanner_create(table_t *table, const char **column_names,
                                      size_t num_columns) {
    table_scanner_t *scanner = calloc(sizeof(*scanner), 1);

    if (!scanner) {
        fprintf(stderr, "Cannot allocate memory for table_scanner_t\n");
        abort();
    }

    scanner->table = table;
    scanner->column_names = column_names;
    scanner->num_columns = num_columns;

    return scanner;
}

block_t table_scanner_next(table_scanner_t *scanner) {
    if (!scanner->finished_disk_parts && scanner->current_part_idx >= scanner->table->num_parts) {
        scanner->finished_disk_parts = true;
    }

    if (scanner->current_readers == NULL && !scanner->finished_disk_parts) {
        scanner->current_readers = malloc(sizeof(column_reader_t *) * scanner->num_columns);

        table_part_t *part = scanner->table->parts[scanner->current_part_idx];
        for (size_t i = 0; i < scanner->num_columns; i++) {
            int32_t col_idx =
                schema_registry_get_column_index(scanner->table->schema, scanner->column_names[i]);
            column_type_t type = scanner->table->schema->columns[col_idx].type;

            scanner->current_readers[i] = column_reader_create(
                part->part_path, scanner->column_names[i], type, scanner->current_mask);
        }
    }

    if (!scanner->finished_disk_parts) {
        block_t block = {0};
        block.num_columns = scanner->num_columns;
        block.columns = malloc(sizeof(slice_i32_t) * scanner->num_columns);

        for (size_t i = 0; i < scanner->num_columns; i++) {
            slice_i32_t slice = column_reader_next_granule_i32(scanner->current_readers[i]);
            block.columns[i] = slice;
            if (i == 0)
                block.num_rows = slice.len;
        }

        if (block.num_rows == 0) {
            scanner->current_part_idx++;
            for (size_t i = 0; i < scanner->num_columns; i++) {
                column_reader_free(scanner->current_readers[i]);
            }
            free(scanner->current_readers);
            scanner->current_readers = NULL;
            free(block.columns);
            return table_scanner_next(scanner);
        } else {
            return block;
        }
    }

    if (!scanner->finished_memory_buffer) {
        scanner->finished_memory_buffer = true;
        if (scanner->table->row_count == 0)
            return (block_t){0};

        block_t block = {0};
        block.num_columns = scanner->num_columns;
        block.columns = malloc(sizeof(slice_i32_t) * scanner->num_columns);

        for (size_t i = 0; i < scanner->num_columns; i++) {
            int32_t col_idx =
                schema_registry_get_column_index(scanner->table->schema, scanner->column_names[i]);
            block.columns[i] = buffer_i32_as_slice(scanner->table->column_buffers[col_idx]);
            if (i == 0)
                block.num_rows = scanner->table->row_count;
        }

        return block;
    }

    return (block_t){0};
}

void table_scanner_destroy(table_scanner_t *scanner) {
    if (!scanner)
        return;
    // Defend against early query cancellation
    if (scanner->current_readers != NULL) {
        for (size_t i = 0; i < scanner->num_columns; i++) {
            column_reader_free(scanner->current_readers[i]);
        }
        free(scanner->current_readers);
    }
    free(scanner);
}
