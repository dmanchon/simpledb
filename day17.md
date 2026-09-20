# Day 17: The Multi-Part Table Scanner (Lazy Data Loading & Granule Pruning)

### Architectural Overview: The Abstraction Gap

Over the past few days, we've built individual components for storage and reading:
1. **Day 11 (`column_reader_t`)**: Reads a single column from a single `.bin` file, skipping granules using a bitmap mask.
2. **Day 14 (`table_t`)**: Manages the ingestion of rows into in-memory buffers and tracks immutable flushed parts on disk.

However, a query engine doesn't ask for a single file. A query engine executes `SELECT user_id, amount FROM events WHERE amount > 100`. It asks the **Table** for a stream of data containing multiple columns at once. 

Because our table uses a MergeTree-like architecture, the data is scattered across:
- Multiple immutable **Parts** on disk (e.g., `part_1`, `part_2`).
- Each part has multiple **Column Files** (`user_id.bin`, `amount.bin`).
- An active **In-Memory Buffer** containing rows that haven't been flushed yet.

To bridge this gap, we introduce the **Table Scanner (`table_scanner_t`)**.

### The Table Scanner Pipeline

The `table_scanner_t` is a pull-based iterator (the Volcano model) that abstracts away the complexity of multiple parts. The execution engine simply calls `table_scanner_next()`, and the scanner yields a `block_t` (a structure containing parallel arrays for the requested columns, up to 8,192 rows at a time).

Under the hood, the scanner acts as an orchestrator:
1. **Part Iteration**: It iterates through the `table_t`'s list of disk parts.
2. **Pruning (Pushdown)**: For each part, it queries the Sparse Primary Index (`primary.idx`) with the query's `WHERE` clause (if any) to generate a `bitmap_t` mask of valid granules.
3. **Column Readers**: It spins up an array of `column_reader_t` instances for the requested columns in that part, passing them the mask.
4. **Yielding Blocks**: It repeatedly pulls granules from the column readers, bundling them into a `block_t`, and yielding them to the query engine.
5. **Memory Buffer**: Once all disk parts are exhausted, it yields one final `block_t` representing the unflushed data currently residing in the table's in-memory buffers.

By doing this, the Query Engine never has to know whether data came from disk or RAM, or how it was pruned!

---

### C Header: The Scanner API

We define `block_t` to represent a generic chunk of columnar data, and `table_scanner_t` to hold the iteration state.

#### `include/simpledb/table_scanner.h`

```c
#pragma once

#include "simpledb/table.h"
#include "simpledb/storage.h"
#include "simpledb/slice.h"
#include <stdbool.h>

// A Block represents a chunk of data yielded by the scanner (e.g., one Granule).
// It contains parallel arrays (slices) for each requested column.
typedef struct {
    size_t num_rows;
    size_t num_columns;
    // For simplicity in this iteration, we assume all requested columns are I32.
    // In a full implementation, this would be an array of polymorphic column views.
    slice_i32_t* columns; 
} block_t;

typedef struct table_scanner_t {
    table_t* table;
    const char** column_names;
    size_t num_columns;
    
    // Iterator state for Disk Parts
    size_t current_part_idx;
    column_reader_t** current_readers;
    bitmap_t* current_mask;
    
    // State flags
    bool finished_disk_parts;
    bool finished_memory_buffer;
} table_scanner_t;

// 1. Create a scanner for specific columns of a table
table_scanner_t* table_scanner_create(table_t* table, const char** column_names, size_t num_columns);

// 2. Yield the next block of data. 
// If block.num_rows == 0, End Of File (EOF) is reached.
block_t table_scanner_next(table_scanner_t* scanner);

// 3. Clean up the scanner
void table_scanner_destroy(table_scanner_t* scanner);
```

---

### Socratic Q&A: The Scanner Implementation

**Q: You mentioned the scanner is a pull-based iterator using the "Volcano model". Why is it called that, who proposed it, and what are the alternatives?**
The Volcano Iterator Model is a standard query execution architecture where every operator (like Scan, Filter, Join) exposes a simple API: `open()`, `next()`, and `close()`. The engine calls `next()` on the top-most operator, which in turn pulls data from its children by calling `next()` on them, passing records up the pipeline. 
- **Who & Why**: It was proposed by **Goetz Graefe** around 1990 ([Volcano—An Extensible and Parallel Query Evaluation System](https://paper.caliper.org/volcano.pdf)). He named it after his extensible database research system at the University of Colorado Boulder, which was literally called "Volcano" (he frequently named projects after natural phenomena, like EXODUS and Cascades). The name became legendary and stuck in the industry.
- **Alternatives**: 
  1. **Materialization (Early Push Model)**: Operators process the *entire* dataset at once and write the full intermediate result to memory before the next operator can start. It is conceptually simple but uses massive amounts of RAM (early MonetDB used this).
  2. **Push-based JIT Compilation (Data-Centric)**: Pioneered by Thomas Neumann for the **HyPer** database ([Efficiently Compiling Efficient Query Plans for Modern Hardware](https://www.vldb.org/pvldb/vol4/p539-neumann.pdf)), this model flips the flow. Instead of operators pulling data via function calls, the database dynamically compiles (JIT) a single, tight CPU `for` loop that pushes data through all operators, keeping data purely in CPU registers and eliminating the overhead of calling `next()`.
While JIT compilation is ultimately faster for complex queries, the **Vectorized Volcano model** (where `next()` yields chunks of 8,192 rows instead of single rows, as we do in `simpledb`) strikes a perfect balance between high performance (SIMD-friendly) and ease of implementation in pure C!


**Q: Why does the scanner take an array of `column_names` instead of just returning all columns in the table?**
This is a crucial optimization known as **Projection Pushdown**. In a columnar database, if a table has 100 columns but the query is `SELECT user_id FROM events`, the scanner should *only* open the `user_id.bin` file. By specifying `column_names` upfront, the scanner avoids doing any disk I/O for the other 99 columns.

**Q: Do I have to open the `.bin` files and implement memory-mapping inside the scanner?**
No! The heavy lifting of the read path was already implemented on Day 11 with `column_reader_t`. The scanner is strictly an orchestrator. It creates an array of `column_reader_t` instances (one for each requested column in `column_names`) and simply calls `column_reader_next_granule_i32()` on them. It bundles those resulting slices into a `block_t` and hands it up to the query engine.

**Q: In `table_scanner_next`, how do we handle moving between parts?**
The state machine in `_next` requires nested loops or checks. 
1. If `current_readers` is NULL, open the readers for `current_part_idx`.
2. Call `column_reader_next_granule_i32` on the first reader. 
3. If it returns 0 rows (EOF for that part), close all `current_readers` (using `column_reader_free`), increment `current_part_idx`, and recursively call `table_scanner_next` to try the next part!

**Q: How do I find the `column_type_t` for a given column name when creating the readers?**
The `table_t` struct holds a pointer to the `schema_registry_t`. You can use `schema_registry_get_column_index(table->schema, col_name)` to find the integer index of the column. Once you have the index, you can look up its type directly from the schema's internal array: `table->schema->columns[col_idx].type`. You can then pass this type straight into `column_reader_create`.

**Q: How does the scanner handle the in-memory buffer at the end?**
Once `current_part_idx` reaches `table->num_parts`, we set `finished_disk_parts = true`. On the next call to `_next`, we check if `finished_memory_buffer` is false. If so, we directly construct a `block_t` by creating `slice_i32_t` views pointing at the `table->column_buffers`, set `finished_memory_buffer = true`, and yield it. Subsequent calls will finally return a 0-row block.

**Q: What is the safest way to initialize the `table_scanner_t` struct?**
Always use `calloc(1, sizeof(table_scanner_t))`. Because the `_next` state machine relies heavily on checking if pointers like `current_readers` and boolean flags like `finished_disk_parts` are `NULL` or `false`, using `malloc` will leave random garbage memory in those fields. This would cause immediate segmentation faults when the state machine runs.

**Q: Why do we pass a `table_t*` into `table_scanner_create`? Doesn't the table create itself?**
Take a close look at the name: it is `table_scanner_create`, not `table_create`! The `table_t` is the permanent storage container (built on Day 14). A `table_scanner_t` is a temporary, read-only **iterator** (or cursor) that scrolls through the data. Because multiple users might be running `SELECT` queries on the same table at the exact same time, each query gets its own independent `table_scanner_t` instance. The scanner needs a pointer to the `table_t` so it knows which table's data to read.

**Q: What exactly is a `block_t`, and why does the scanner return it instead of a single row?**
In a traditional row-based database (like PostgreSQL or MySQL), the scanner yields data one single row at a time. If you query 10 million rows, the engine has to execute the `next()` function 10 million times! This function call overhead destroys CPU cache efficiency and branch prediction.
In a **Vectorized engine** (like ClickHouse, DuckDB, or `simpledb`), operators pass data in **Blocks**. A `block_t` is a lightweight struct that contains a vertical slice of the table: it holds an array of `slice_i32_t`, where each slice represents a full column of up to 8,192 contiguous rows. 
By yielding a `block_t`, we only call `next()` once per 8,192 rows. The execution engine can then take those contiguous arrays and feed them directly into hardware SIMD instructions, processing thousands of values in a fraction of a microsecond!

**Q: When building the block, is it safe to just set `block.num_rows` based on the length of the very first column's slice?**
Yes! While it might feel "rough", it relies on the most fundamental mathematical invariant of a Columnar Database: **every column in a part MUST have the exact same number of rows.** If column `cpu` yields 8,192 rows, column `ram` must yield exactly 8,192 rows. If they don't, the database is physically corrupted. Production databases (like ClickHouse) do exactly this, but they wrap it in defensive `assert()` checks to crash immediately if the invariant is ever violated.

---

### Unit Testing the Scanner

We will simulate a table with flushed parts and un-flushed memory buffers, and verify that the scanner seamlessly reads across both domains.

#### `tests/test_db.c`

```c
static void test_table_scanner(void) {
    printf("Testing Multi-Part Table Scanner...\n");
    
    // 1. Setup a table and schema
    table_t* table = table_create("metrics");
    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "cpu", COL_TYPE_I32);
    schema_registry_add_column(schema, "ram", COL_TYPE_I32);
    table->schema = schema;
    
    // Allocate polymorphic buffers
    table->column_buffers = calloc(2, sizeof(void*));
    table->column_buffers[0] = buffer_i32_create(10);
    table->column_buffers[1] = buffer_i32_create(10);
    
    // 2. Insert data into memory
    int32_t cpu_vals[] = { 10, 20, 30 };
    int32_t ram_vals[] = { 100, 200, 300 };
    const void* batch1[] = { cpu_vals, ram_vals };
    table_insert_batch(table, batch1, 2, 3);
    
    // 3. (Mock) Flush to part 0
    // In a real test, we would call table_flush(table, "."), but we assume it's mocked here 
    // to create disk files, and then we insert more data to remain in memory.
    
    // 4. Create Scanner for only the "cpu" column (Projection Pushdown)
    const char* cols[] = { "cpu" };
    table_scanner_t* scanner = table_scanner_create(table, cols, 1);
    assert(scanner != NULL);
    
    // 5. Scan blocks
    size_t total_rows_scanned = 0;
    while (true) {
        block_t block = table_scanner_next(scanner);
        if (block.num_rows == 0) break;
        
        assert(block.num_columns == 1);
        total_rows_scanned += block.num_rows;
        
        // Free block slices if necessary according to your implementation
        free(block.columns);
    }
    
    // In our mock, if we scanned the memory buffer, we should see 3 rows.
    assert(total_rows_scanned == 3);
    
    table_scanner_destroy(scanner);
    table_destroy(table);
    printf("Table Scanner tests passed!\n");
}
```

---

## The Final Listing

For reference, here is the complete, final implementation of the Table Scanner.

#### `src/table_scanner.c`

```c
#include "simpledb/table_scanner.h"
#include "simpledb/slice.h"
#include "simpledb/storage.h"
#include "simpledb/table.h"
#include <_abort.h>
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
        block_t block;
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

        block_t block;
        block.num_columns = scanner->num_columns;
        block.columns = malloc(sizeof(slice_i32_t) * scanner->num_columns);

        for (size_t i = 0; i < scanner->num_columns; i++) {
            block.columns[i] = buffer_i32_as_slice(scanner->table->column_buffers[i]);
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
```
