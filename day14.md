---
title: "Day 14: The Table Abstraction (Ingestion Layer)"
pubDate: 2026-08-27
description: "Building the coordinating ingestion layer to multiplex rows into columnar files and update indexes simultaneously."
---

# Day 14: The Table Abstraction (Ingestion Layer)

## Architectural Explanation

Up until now, we've built individual components: memory-mapped columns, string dictionaries, null bitmaps, and sparse primary indexes. However, a database user doesn't insert data column by column; they insert *rows* (or records) into a *table*.

The **Table Abstraction (`table_t`)** acts as the ingestion coordinator. When a new row arrives (for example, parsed from a JSON event), the table layer must:
1. Multiplex the incoming fields into their respective columnar buffers.
2. Manage the dynamic schema registry if a new column appears.
3. Coordinate the updating of secondary data structures, such as the string dictionary and the inverted text index.
4. Manage memory limits, eventually triggering a flush to the on-disk `.bin` and `.mrk` files.

This layer bridges the gap between row-oriented ingestion and columnar storage.

## C Header (`include/table.h`)

```c
#ifndef SIMPLEDB_TABLE_H
#define SIMPLEDB_TABLE_H

#include <stddef.h>
#include <stdint.h>

// Forward declarations
typedef struct schema_registry_t schema_registry_t;
typedef struct fts_index_t fts_index_t;

typedef struct table_t {
    char name[64];
    schema_registry_t* schema;
    
    // In-memory columnar buffers (one per column in schema)
    void** column_buffers; 
    
    // Total rows currently in memory
    size_t row_count;
    
    // Optional FTS index for text columns
    fts_index_t* fts;
} table_t;

// Initialize a new table structure
table_t* table_create(const char* name);

// Free the table and its in-memory buffers
void table_destroy(table_t* table);

// Insert a generic 'row' represented as an array of parsed values.
int table_insert_row(table_t* table, const void** values, size_t num_values);

// Flush current in-memory buffers to disk as a new part/granule
int table_flush(table_t* table, const char* data_dir);

#endif // SIMPLEDB_TABLE_H
```

## Unit Tests (`tests/test_table.c`)

```c
#include "table.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

void test_table_creation() {
    table_t* t = table_create("events");
    assert(t != NULL);
    assert(strcmp(t->name, "events") == 0);
    assert(t->row_count == 0);
    
    table_destroy(t);
    printf("test_table_creation passed.\n");
}

int main() {
    test_table_creation();
    return 0;
}
```
