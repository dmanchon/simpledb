#include "simpledb/table.h"
#include "simpledb/allocator.h"
#include "simpledb/fts.h"
#include "simpledb/schema_registry.h"
#include "simpledb/storage.h"
#include "simpledb/string_dict.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

table_t *table_create(const char *name) {
    table_t *table = malloc(sizeof(*table));

    strncpy(table->name, name, sizeof(table->name) - 1);
    table->name[63] = '\0';

    table->num_parts = 0;
    table->row_count = 0;
    table->column_buffers = NULL;
    table->parts = NULL;
    table->schema = NULL;
    table->fts = NULL;

    return table;
}

void table_destroy(table_t *table) {
    if (!table)
        return;

    if (table->column_buffers && table->schema) {
        for (size_t i = 0; i < table->schema->count; i++) {
            if (!table->column_buffers[i])
                continue;

            switch (table->schema->columns[i].type) {
            case COL_TYPE_I32:
            case COL_TYPE_F32: {
                buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
                buffer_i32_free(buf);
                break;
            }
            case COL_TYPE_STRING_DICT: {
                string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
                string_dict_free((string_dict_t *)col->dict);
                buffer_i32_free(col->keys_buf);
                free(col);
                break;
            }
            case COL_TYPE_STRING:
                raw_string_column_free((raw_string_column_t *)table->column_buffers[i]);
                break;
            }
        }
    }
    free(table->column_buffers);

    if (table->fts) {
        inverted_index_free(table->fts);
    }

    for (size_t i = 0; i < table->num_parts; i++) {
        free(table->parts[i]);
    }
    if (table->parts)
        free(table->parts);

    schema_registry_free(table->schema);

    free(table);
}

int table_insert_batch(table_t *table, const void ***columnar_values, size_t num_columns,
                       size_t num_rows) {
    if (!table || !table->schema || !table->column_buffers) {
        return -1;
    }
    if (num_columns != table->schema->count) {
        return -1;
    }
    for (size_t i = 0; i < table->schema->count; i++) {
        if (!table->column_buffers[i])
            continue;

        switch (table->schema->columns[i].type) {
        case COL_TYPE_I32: {
            buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
            buffer_i32_ensure_capacity(buf, num_rows + buf->size);
            memcpy((int32_t *)buf->data + buf->size, columnar_values[i],
                   sizeof(int32_t) * num_rows);
            buf->size += num_rows;
            break;
        }
        case COL_TYPE_F32: {
            buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
            buffer_i32_ensure_capacity(buf, num_rows + buf->size);
            memcpy((float *)buf->data + buf->size, columnar_values[i], sizeof(float) * num_rows);
            buf->size += num_rows;
            break;
        }
        case COL_TYPE_STRING_DICT: {
            string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
            string_dict_encode_column(col->dict, (const char **)columnar_values[i], num_rows,
                                      col->keys_buf);
            break;
        }

        case COL_TYPE_STRING: {
            raw_string_column_t *col = (raw_string_column_t *)table->column_buffers[i];
            if (col->size + num_rows > col->capacity) {
                size_t new_cap = (col->capacity == 0) ? 128 : col->capacity * 2;
                while (new_cap < col->size + num_rows) {
                    new_cap *= 2;
                }
                col->strings = realloc(col->strings, sizeof(char *) * new_cap);
                if (!col->strings) {
                    fprintf(stderr, "[FATAL ERROR] string column failed to allocate\n");
                    exit(EXIT_FAILURE);
                }
                col->capacity = new_cap;
            }
            for (size_t r = 0; r < num_rows; r++) {
                col->strings[col->size + r] = strdup(((const char **)columnar_values[i])[r]);
            }
            col->size += num_rows;
            break;
        }
        }
    }
    table->row_count += num_rows;
    return 0;
}

static void write_string_pool(FILE *fp, char **strings, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fwrite(strings[i], 1, strlen(strings[i]) + 1, fp);
    }
}

int table_flush(table_t *table, const char *data_dir) {
    if (!table || !table->schema || !table->column_buffers || table->row_count == 0) {
        return 0; // Nothing to flush
    }

    for (size_t i = 0; i < table->schema->count; i++) {
        if (!table->column_buffers[i])
            continue;

        column_writer_t *writer = column_writer_create(data_dir, table->schema->columns[i].name,
                                                       table->schema->columns[i].type);
        switch (table->schema->columns[i].type) {
        case COL_TYPE_I32:
        case COL_TYPE_F32: {
            buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
            column_writer_append(writer, buf->data, table->row_count);
            buf->size = 0;
            break;
        }
        case COL_TYPE_STRING_DICT: {
            // first write the dict keys
            string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
            column_writer_append(writer, col->keys_buf->data, table->row_count);
            col->keys_buf->size = 0;
            // then write the original strings
            char dict_path[256];
            snprintf(dict_path, sizeof(dict_path), "%s/%s.dict", data_dir,
                     table->schema->columns[i].name);
            FILE *dfp = fopen(dict_path, "wb");
            if (!dfp) {
                fprintf(stderr, "[FATAL ERROR] cannot open file %s\n", dict_path);
                exit(EXIT_FAILURE);
            }
            write_string_pool(dfp, col->dict->strings, col->dict->count);
            fclose(dfp);
            break;
        }

        case COL_TYPE_STRING: {
            raw_string_column_t *col = (raw_string_column_t *)table->column_buffers[i];
            column_writer_append(writer, col->strings, col->size);
            for (size_t r = 0; r < col->size; r++) {
                free(col->strings[r]);
            }
            col->size = 0;
            break;
        }
        }
        column_writer_close(writer);
    }
    // Register the new immutable part in table->parts
    table_part_t **new_parts = realloc(table->parts, sizeof(table_part_t *) * (table->num_parts + 1));
    if (!new_parts) return -1;
    table->parts = new_parts;
    table_part_t *part = malloc(sizeof(table_part_t));
    if (!part) return -1;
    strncpy(part->part_path, data_dir, sizeof(part->part_path) - 1);
    part->part_path[sizeof(part->part_path) - 1] = '\0';
    part->row_count = table->row_count;
    table->parts[table->num_parts++] = part;
    // Reset table row count
    table->row_count = 0;
    return 0;
}
table_part_t **table_get_parts(table_t *table, size_t *out_num_parts) {
    if (!table) {
        if (out_num_parts) *out_num_parts = 0;
        return NULL;
    }
    if (out_num_parts) *out_num_parts = table->num_parts;
    return table->parts;
}
void **table_get_active_buffer(table_t *table, size_t *out_row_count) {
    if (!table) {
        if (out_row_count) *out_row_count = 0;
        return NULL;
    }
    if (out_row_count) *out_row_count = table->row_count;
    return table->column_buffers;
}
