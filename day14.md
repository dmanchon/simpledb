# Day 14: The Table Abstraction (Ingestion Layer)

### Architectural Overview

Up until now, we've built specialized, isolated components: 64-byte aligned SIMD buffers, memory-mapped files, string dictionaries, and sparse primary indexes. However, a database user does not insert data column by column; they insert *records* into a **Table**.

The **Table Abstraction (`table_t`)** serves as the central ingestion coordinator. When a batch of rows arrives, the table layer must:
1. **Multiplex** incoming records into their respective columnar memory buffers.
2. **Coordinate** secondary data structures (such as string dictionary encoding and full-text inverted indexes).
3. **Manage** in-memory buffer capacities.
4. **Trigger flushes** to serialize immutable `.bin` and `.mrk` parts to disk once memory thresholds are met.

---

### Ingestion vs. Read Path: Why Decouple Them?

In classical row-oriented databases (like PostgreSQL), a table is a monolith that handles reads, writes, and indexing simultaneously. In analytical engines (like ClickHouse's MergeTree), the write path and read path are strictly decoupled:

1. **Insert (`table_insert_batch`):** Writes strictly to **in-memory columnar buffers**. Operating purely in RAM allows the engine to achieve millions of rows/sec ingestion throughput.
2. **Flush (`table_flush`):** When RAM fills up, the in-memory buffers are serialized sequentially into a permanent, **immutable disk part** (or granule). Once written, the memory buffers are cleared for the next batch.

```text
       Ingestion (Fast RAM)                  On-Disk Storage (Immutable)
 ┌───────────────────────────────┐           ┌───────────────────────────┐
 │ table_insert_batch()          │           │ Part 001 (.bin / .mrk)    │
 │ (Multiplexes into RAM buffers)│           ├───────────────────────────┤
 └──────────────┬────────────────┘           │ Part 002 (.bin / .mrk)    │
                │ table_flush()              ├───────────────────────────┤
                └───────────────────────────►│ Part 003 (.bin / .mrk)    │
                                             └───────────────────────────┘
```

*Why do it this way?* Disks (even high-end NVMe SSDs) perform poorly with thousands of tiny, random writes. By batching writes in memory and performing one massive **sequential disk write** during flush, we maximize physical hardware I/O bandwidth.

Reading across these fragmented immutable disk parts and live memory buffers is delegated to a separate `Scanner` / `QueryEngine` pipeline.

---

### The "Loop Flip" & Batch Ingestion

Ingesting data row-by-row creates a hidden performance bottleneck:
* If you insert 10 million rows across 10 columns, the database must evaluate the schema type **100 million times** inside the inner loop.
* This constant branching thrashes the CPU's branch predictor, creating severe "interpretation overhead."

To eliminate this, we perform the **"Loop Flip"**:

```text
Row-by-Row Ingestion (Slow):
For each ROW:
    For each COLUMN:
        Switch(column.type) ──► Branch evaluated 100,000,000 times!

Columnar Batch Ingestion (Fast):
For each COLUMN:
    Switch(column.type)     ──► Branch evaluated only ONCE per column!
        Bulk copy thousands of rows with zero branching!
```

---

### In-Memory Polymorphic Column Architecture

In C, columns cannot share a single base class. We store them as an array of opaque pointers (`void** column_buffers`), dynamically dispatched via `schema_registry_t`.

```text
table_t->column_buffers
 ├── [0] (COL_TYPE_I32)         ──► buffer_i32_t (Contiguous int32_t array)
 ├── [1] (COL_TYPE_F32)         ──► buffer_i32_t (Contiguous float array)
 ├── [2] (COL_TYPE_STRING_DICT) ──► string_dict_column_t { dict, keys_buf }
 └── [3] (COL_TYPE_STRING)      ──► raw_string_column_t { char** strings }
```

#### 1. Primitive Numerics (`COL_TYPE_I32` & `COL_TYPE_F32`)
Because incoming numbers are already contiguous arrays, we skip row iteration completely and execute a single hardware-vectorized `memcpy()` directly to the buffer's tail offset:

```c
case COL_TYPE_I32: {
    buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
    buffer_i32_ensure_capacity(buf, num_rows + buf->size);
    memcpy((int32_t *)buf->data + buf->size, columnar_values[i], sizeof(int32_t) * num_rows);
    buf->size += num_rows;
    break;
}
```

#### 2. Dictionary-Encoded Strings (`COL_TYPE_STRING_DICT`)
Incoming string arrays (`const char**`) are bulk-encoded through our hash-indexed dictionary, deduplicating entries and appending 32-bit surrogate integer keys:

```c
case COL_TYPE_STRING_DICT: {
    string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
    string_dict_encode_column(col->dict, (const char **)columnar_values[i], num_rows, col->keys_buf);
    break;
}
```

#### 3. Variable-Length Raw Strings (`COL_TYPE_STRING`)
For uncompressed text, shallow-copying pointers with `memcpy` is dangerous because the table's destructor will eventually call `free()` on string pointers that may point to read-only string literals or stack memory. We deep-copy every string using `strdup()`:

```c
case COL_TYPE_STRING: {
    raw_string_column_t *col = (raw_string_column_t *)table->column_buffers[i];
    if (col->size + num_rows > col->capacity) {
        size_t new_cap = (col->capacity == 0) ? 128 : col->capacity * 2;
        while (new_cap < col->size + num_rows) new_cap *= 2;
        col->strings = realloc(col->strings, sizeof(char *) * new_cap);
        col->capacity = new_cap;
    }
    for (size_t r = 0; r < num_rows; r++) {
        col->strings[col->size + r] = strdup(((const char **)columnar_values[i])[r]);
    }
    col->size += num_rows;
    break;
}
```

---

### Symmetrical Destruction (The Reverse Loop Flip)

To cleanly destroy a table without leaking memory, we must iterate through the schema in reverse, casting each opaque buffer back to its concrete container before freeing the pointer array:

```c
void table_destroy(table_t *table) {
    if (!table) return;

    if (table->column_buffers && table->schema) {
        for (size_t i = 0; i < table->schema->count; i++) {
            if (!table->column_buffers[i]) continue;

            switch (table->schema->columns[i].type) {
            case COL_TYPE_I32:
            case COL_TYPE_F32:
                buffer_i32_free((buffer_i32_t *)table->column_buffers[i]);
                break;
            case COL_TYPE_STRING_DICT: {
                string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
                string_dict_free(col->dict);
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
    if (table->fts) inverted_index_free(table->fts);
    for (size_t i = 0; i < table->num_parts; i++) free(table->parts[i]);
    if (table->parts) free(table->parts);
    schema_registry_free(table->schema);
    free(table);
}
```

---

## C Header (`include/simpledb/table.h`)

```c
#ifndef SIMPLEDB_TABLE_H
#define SIMPLEDB_TABLE_H

#include <stddef.h>
#include <stdint.h>
#include "simpledb/fts.h"
#include "simpledb/schema_registry.h"

typedef struct table_part_t {
    char part_path[256];
    size_t row_count;
} table_part_t;

typedef struct table_t {
    char name[64];
    schema_registry_t *schema;
    void **column_buffers;
    size_t row_count;
    table_part_t **parts;
    size_t num_parts;
    inverted_index_t *fts;
} table_t;

typedef struct {
    string_dict_t *dict;
    buffer_i32_t *keys_buf;
} string_dict_column_t;

typedef struct {
    char **strings;
    size_t size;
    size_t capacity;
} raw_string_column_t;

static inline raw_string_column_t *raw_string_column_create(size_t initial_capacity) {
    raw_string_column_t *col = calloc(1, sizeof(raw_string_column_t));
    if (!col) return NULL;
    if (initial_capacity > 0) {
        col->strings = malloc(initial_capacity * sizeof(char *));
        col->capacity = initial_capacity;
    }
    return col;
}

static inline void raw_string_column_free(raw_string_column_t *col) {
    if (!col) return;
    if (col->strings) {
        for (size_t r = 0; r < col->size; r++) free(col->strings[r]);
        free(col->strings);
    }
    free(col);
}

// --- Write Path (Ingestion) ---
table_t *table_create(const char *name);
void table_destroy(table_t *table);
int table_insert_batch(table_t *table, const void ***columnar_values, size_t num_columns, size_t num_rows);
int table_flush(table_t *table, const char *data_dir);

// --- Read Path (Metadata API) ---
table_part_t **table_get_parts(table_t *table, size_t *out_num_parts);
void **table_get_active_buffer(table_t *table, size_t *out_row_count);

#endif // SIMPLEDB_TABLE_H
```

---

## End-to-End Verification (`tests/test_db.c`)

```c
void test_table_e2e_batch_ingestion(void) {
    printf("Testing Table E2E Batch Ingestion...\n");

    // 1. Create table & setup 4-column schema
    table_t* table = table_create("user_events");
    assert(table != NULL);

    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "score", COL_TYPE_F32);
    schema_registry_add_column(schema, "country", COL_TYPE_STRING_DICT);
    schema_registry_add_column(schema, "comment", COL_TYPE_STRING);
    table->schema = schema;

    // Allocate polymorphic buffers
    table->column_buffers = calloc(schema->count, sizeof(void*));
    table->column_buffers[0] = buffer_i32_create(4);
    table->column_buffers[1] = buffer_i32_create(4);

    string_dict_column_t* sdc = malloc(sizeof(string_dict_column_t));
    sdc->dict = string_dict_create();
    sdc->keys_buf = buffer_i32_create(4);
    table->column_buffers[2] = sdc;

    table->column_buffers[3] = raw_string_column_create(4);

    // 2. Batch 1: Ingest 3 rows
    int32_t b1_user_ids[] = { 101, 102, 103 };
    float   b1_scores[]   = { 95.5f, 82.0f, 99.1f };
    const char* b1_countries[] = { "US", "ES", "US" };
    const char* b1_comments[]  = { "First event", "Second event", "Third event" };

    const void** batch1[4];
    batch1[0] = (const void*)b1_user_ids;
    batch1[1] = (const void*)b1_scores;
    batch1[2] = (const void*)b1_countries;
    batch1[3] = (const void*)b1_comments;

    assert(table_insert_batch(table, (const void***)batch1, 4, 3) == 0);
    assert(table->row_count == 3);

    // 3. Batch 2: Ingest 2 more rows to test tail offset appending & capacity growth
    int32_t b2_user_ids[] = { 104, 105 };
    float   b2_scores[]   = { 77.4f, 88.9f };
    const char* b2_countries[] = { "FR", "ES" };
    const char* b2_comments[]  = { "Fourth event", "Fifth event" };

    const void** batch2[4];
    batch2[0] = (const void*)b2_user_ids;
    batch2[1] = (const void*)b2_scores;
    batch2[2] = (const void*)b2_countries;
    batch2[3] = (const void*)b2_comments;

    assert(table_insert_batch(table, (const void***)batch2, 4, 2) == 0);
    assert(table->row_count == 5);

    // 4. Verify Column 0 (I32 user_id)
    buffer_i32_t* c0 = (buffer_i32_t*)table->column_buffers[0];
    assert(c0->size == 5 && c0->data[0] == 101 && c0->data[4] == 105);

    // 5. Verify Column 1 (F32 score)
    buffer_i32_t* c1 = (buffer_i32_t*)table->column_buffers[1];
    assert(c1->size == 5 && ((float*)c1->data)[0] == 95.5f && ((float*)c1->data)[4] == 88.9f);

    // 6. Verify Column 2 (STRING_DICT country)
    string_dict_column_t* c2 = (string_dict_column_t*)table->column_buffers[2];
    assert(c2->keys_buf->size == 5);
    assert(c2->keys_buf->data[0] == 0 && c2->keys_buf->data[1] == 1 && c2->keys_buf->data[2] == 0);

    // 7. Verify Column 3 (Raw String comment)
    raw_string_column_t* c3 = (raw_string_column_t*)table->column_buffers[3];
    assert(c3->size == 5);
    assert(strcmp(c3->strings[0], "First event") == 0 && strcmp(c3->strings[4], "Fifth event") == 0);

    // 8. Teardown
    table_destroy(table);
    printf("Table E2E Batch Ingestion tests passed!\n");
}
```

---

## Final Implementation (`src/table.c`)

```c
#include "simpledb/table.h"
#include "simpledb/allocator.h"
#include "simpledb/fts.h"
#include "simpledb/schema_registry.h"
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
    if (!table) return;

    if (table->column_buffers && table->schema) {
        for (size_t i = 0; i < table->schema->count; i++) {
            if (!table->column_buffers[i]) continue;

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
    if (table->fts) inverted_index_free(table->fts);
    for (size_t i = 0; i < table->num_parts; i++) free(table->parts[i]);
    if (table->parts) free(table->parts);
    schema_registry_free(table->schema);
    free(table);
}

int table_insert_batch(table_t *table, const void ***columnar_values, size_t num_columns, size_t num_rows) {
    if (!table || !table->schema || !table->column_buffers || num_columns != table->schema->count) {
        return -1;
    }
    for (size_t i = 0; i < table->schema->count; i++) {
        if (!table->column_buffers[i]) continue;

        switch (table->schema->columns[i].type) {
        case COL_TYPE_I32: {
            buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
            buffer_i32_ensure_capacity(buf, num_rows + buf->size);
            memcpy((int32_t *)buf->data + buf->size, columnar_values[i], sizeof(int32_t) * num_rows);
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
            string_dict_encode_column(col->dict, (const char **)columnar_values[i], num_rows, col->keys_buf);
            break;
        }
        case COL_TYPE_STRING: {
            raw_string_column_t *col = (raw_string_column_t *)table->column_buffers[i];
            if (col->size + num_rows > col->capacity) {
                size_t new_cap = (col->capacity == 0) ? 128 : col->capacity * 2;
                while (new_cap < col->size + num_rows) new_cap *= 2;
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

int table_flush(table_t *table, const char *data_dir) {
    fprintf(stderr, "table_flush not implemented\n");
    abort();
}

table_part_t **table_get_parts(table_t *table, size_t *out_num_parts) {
    fprintf(stderr, "table_get_parts not implemented\n");
    abort();
}

void **table_get_active_buffer(table_t *table, size_t *out_row_count) {
    fprintf(stderr, "table_get_active_buffer not implemented\n");
    abort();
}
```
