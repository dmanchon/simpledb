#ifndef SIMPLEDB_TABLE_H
#define SIMPLEDB_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "simpledb/fts.h"
#include "simpledb/schema_registry.h"

// A simple structure representing an immutable flushed part on disk
typedef struct table_part_t {
    char part_path[256];
    size_t row_count;
} table_part_t;

typedef struct table_t {
    char name[64];
    schema_registry_t *schema;

    // In-memory columnar buffers (one per column in schema)
    void **column_buffers;

    // Total rows currently in memory
    size_t row_count;

    // Track flushed parts (for the Read API)
    table_part_t **parts;
    size_t num_parts;

    // Optional FTS index for text columns
    inverted_index_t *fts;
} table_t;

typedef struct {
    string_dict_t *dict;
    buffer_i32_t *keys_buf;
} string_dict_column_t;

typedef struct {
    char **strings;  // Array of dynamically allocated string pointers
    size_t size;     // Current number of strings
    size_t capacity; // Allocated pointer capacity
} raw_string_column_t;

static inline raw_string_column_t *raw_string_column_create(size_t initial_capacity) {
    raw_string_column_t *col = calloc(1, sizeof(raw_string_column_t));
    if (!col)
        return NULL;
    if (initial_capacity > 0) {
        col->strings = malloc(initial_capacity * sizeof(char *));
        col->capacity = initial_capacity;
    }
    return col;
}
// Destructor
static inline void raw_string_column_free(raw_string_column_t *col) {
    if (!col)
        return;
    if (col->strings) {
        for (size_t r = 0; r < col->size; r++) {
            free(col->strings[r]);
        }
        free(col->strings);
    }
    free(col);
}
// --- Write Path (Ingestion) ---

// Initialize a new table structure
table_t *table_create(const char *name);

// Free the table and its in-memory buffers
void table_destroy(table_t *table);

// Insert a columnar batch of data.
// columnar_values[c] is an array of data for column 'c' (e.g. an int64_t* array)
// This avoids row-by-row type branching!
int table_insert_batch(table_t *table, const void ***columnar_values, size_t num_columns,
                       size_t num_rows);

// Flush current in-memory buffers to disk as a new part/granule
int table_flush(table_t *table, const char *data_dir);

// --- Read Path (Scanner Metadata API) ---

// Retrieve all immutable flushed parts on disk
table_part_t **table_get_parts(table_t *table, size_t *out_num_parts);

// Retrieve the active in-memory columnar buffers
void **table_get_active_buffer(table_t *table, size_t *out_row_count);

#endif // SIMPLEDB_TABLE_H
