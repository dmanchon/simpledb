# Day 14: The Table Abstraction (Ingestion Layer)

### Architectural Explanation

Up until now, we've built individual components: memory-mapped columns, string dictionaries, null bitmaps, and sparse primary indexes. However, a database user doesn't insert data column by column; they insert *rows* (or records) into a *table*.

The **Table Abstraction (`table_t`)** acts as the ingestion coordinator. When a new batch of rows arrives (for example, parsed from a JSON event stream), the table layer must:
1. Multiplex the incoming fields into their respective columnar buffers.
2. Manage the dynamic schema registry if a new column appears.
3. Coordinate the updating of secondary data structures, such as the string dictionary and the inverted text index.
4. Manage memory limits, eventually triggering a flush to the on-disk `.bin` and `.mrk` files.

This layer bridges the gap between row-oriented ingestion and columnar storage.

### How Databases Handle Schema Evolution

When a row arrives with fields that aren't in the current schema, databases generally take one of three approaches:

1. **Strict Schema-on-Write (e.g., PostgreSQL, MySQL):** 
   The database immediately rejects the row. The user must execute an explicit `ALTER TABLE ADD COLUMN` command before the system will accept the new data. This guarantees data integrity but makes ingesting dynamic data (like application logs or JSON payloads) cumbersome.

2. **Semi-Dynamic / JSON Columns (e.g., ClickHouse, DuckDB):**
   The schema is mostly rigid, but allows for a special `JSON` or `Map` column type. Any unexpected fields in the incoming row are bundled together and dumped into this single unstructured column. This avoids rejecting the row, but querying the unstructured data later is slower because it can't fully utilize columnar optimizations like vectorization or sparse indexing.

3. **Fully Dynamic Schema (e.g., Honeycomb, Elasticsearch, `simpledb`):**
   The database embraces schema-less ingestion. When a new field is detected in an incoming row, the database *automatically* and transparently updates its internal schema registry. It immediately allocates a new columnar buffer for that specific field and backfills NULLs (or zeroes) for all previously ingested rows. This provides the best of both worlds: zero-friction ingestion while still maintaining strict columnar structures under the hood for blazing-fast queries. 

In `simpledb`, we are aiming for the **Fully Dynamic Schema** approach. The `table_t` abstraction will detect new fields during ingestion and automatically register them with the `schema_registry_t`.

### Where is the Read Path? (Insert vs. Flush)

You might have noticed that `table_t` does not have a `scan`, `read`, or `query` method to actually evaluate queries. Instead, it only provides metadata getters (like `table_get_parts`) to hand off its state. This separation of concerns is a fundamental design pattern in OLAP databases (like ClickHouse's MergeTree).

Here is how the data lifecycle works in this architecture:
1. **Insert:** When you call `table_insert_batch`, the data is strictly written to the **in-memory** `column_buffers`. We do not touch the disk. This makes ingestion lightning-fast.
2. **Flush:** Eventually, memory fills up. We call `table_flush` to serialize the current in-memory buffers into a permanent, **immutable** set of files on disk (often called a "part" or "granule"). Once flushed, the in-memory buffers are cleared and ready for new inserts.

*Why do it this way?* Disks (even fast NVMe SSDs) are terrible at handling thousands of tiny, random writes. By keeping inserts strictly in memory, we batch up all those tiny writes. When we finally `flush`, we perform one massive, contiguous **Sequential Write** to disk, which operates at the hardware's maximum physical bandwidth.

**Why decouple Query Execution?**
Because data is constantly being flushed into new immutable files, a single table might consist of an active in-memory buffer plus dozens of flushed parts on disk. 
If we put a `read` method directly on the table, it would have to do an enormous amount of work: scanning memory, opening multiple disk files, merging results, and evaluating filters. 

Instead, OLAP databases use a separate abstraction (often called a `Scanner`, `BlockInputStream`, or `QueryEngine`) for reading. The Table's job is just to manage the data lifecycle (ingest -> memory -> flush to disk -> merge files). The Scanner's job is to read across all those pieces.

### How ClickHouse Handles This (The MergeTree Engine)

Yes, this is exactly what ClickHouse does. It is the defining characteristic of their core storage engine family: **MergeTree**.

In ClickHouse:
1. **Inserts are just Part Creations:** When you `INSERT` into a MergeTree table, ClickHouse quickly writes that batch of data as a brand-new, immutable "part" on disk (or in memory first for modern async inserts). It does not try to append to existing files.
2. **Background Merges:** Because doing 1,000 inserts would create 1,000 separate parts (which would ruin read performance), a background process constantly wakes up, grabs a few of these smaller parts, and merges them into a single larger part. This is why it's called a *Merge*Tree.
3. **The Read Abstraction (`Processors` / `Pipelines`):** When you run a `SELECT` query, ClickHouse does not ask the Table object to evaluate the query. Instead, it builds a query execution pipeline (using the `IProcessor` interface). It spins up multiple read streams—some reading from the active memory buffers, and others reading from the various flushed disk parts. These streams output generic "Blocks" of columnar data. The query engine just pulls these Blocks through the pipeline (filtering and aggregating them) completely agnostic to whether the data came from file A, file B, or active memory.

This heavily decoupled pipeline approach is what allows ClickHouse to parallelize queries so aggressively across CPU cores.

### The Evolution of Column Types: Why Bottom-Up Design Works

You might wonder: *Why didn't we design all these polymorphic column types (`I32`, `F32`, `STRING_DICT`, `STRING`) on Day 1?*

This deliberate progression mirrors how real-world analytical database kernels (like ClickHouse, DuckDB, and MonetDB) are designed from scratch:

1. **Isolation of Core Mechanical Invariants (Days 1–5):**
   If you try to build hardware-level 64-byte SIMD alignment, zero-copy `mmap`, vector compression, and sparse index granules while simultaneously juggling polymorphic dynamic typing, the cognitive and architectural complexity explodes. By isolating the kernel to a pure 4-byte primitive (`int32_t`), we proved our hardware vector performance and index mechanics in their purest form.

2. **Incremental Storage Specialization (Days 6–13):**
   Once the numeric engine was proven, we introduced distinct storage techniques: surrogate dictionary mapping (Day 6), full-text inverted indexes (Day 8), offset-based variable-length string files (Day 10), and roaring deletion vectors (Day 13).

3. **The Unification Layer (Day 14 - The Table):**
   The Table is the very first time all these independent components are brought under one roof. The need for polymorphic buffers (`void** column_buffers`), schema dispatching, and custom container destruction is not an afterthought—it is the natural consequence of unifying specialized storage engines into a coherent user-facing database.

### Is "Table" the Right Name?

Given that this abstraction only handles memory buffers, schema tracking, and flushing—but *not* reading—is `table_t` actually a good name for it?

Philosophically, you could argue it is technically a **`StorageEngine`**, an **`IngestionCoordinator`**, or a **`PartitionManager`**. 

In older monolithic databases (like PostgreSQL), "Table" is an overloaded term that represents the schema, the storage files, the B-Trees, and the query target all rolled into one. 

In ClickHouse's C++ source code, what the user calls a "Table" is actually implemented via an interface called `IStorage`, and the specific implementation is called `StorageMergeTree`. 

However, we keep the name `table_t` for a very pragmatic reason: **User Ergonomics**. 
When a user writes an application, their mental model is: `"I want to INSERT this row into the 'events' Table."` 

Naming the target of the write path `table_t` perfectly aligns with the API consumer's mental model, even if under the hood it is simply acting as an ingestion buffer that creates immutable disk parts. The query engine will later use this same `table_t` struct simply as a metadata registry to figure out where to find the on-disk parts it needs to read.

### What About the Read API?

If the query engine (Scanner) needs to ask the `table_t` for its list of flushed files and active memory buffers, why aren't those methods in the header yet?

Because we are building this database iteratively! Our immediate focus for Day 14 is strictly the **Write Path**: successfully taking incoming rows, dynamically registering their schema, buffering them in memory, and flushing them to disk. 

Once our ingestion pipeline is rock solid, we will return to `table.h` in a future day to add the Read Metadata API (e.g., methods like `table_get_parts()` or `table_get_active_buffer()`) so the Scanner can do its job.

### Why `void**` for Column Buffers?

If we already spent previous days building robust representations for our columns (like string dictionaries, integer columns, etc.), why does `table_t` declare its buffers as a generic `void** column_buffers;`?

This comes down to how we handle **Polymorphism in C**. 

In a database table, every column can be a completely different data type. Column 0 might be an `int64_column_t*`, Column 1 might be a `string_column_t*`, and Column 2 might be a `float_column_t*`. 

Because C does not have native generics or object-oriented inheritance, we have two primary ways to store an array of mixed column structs:
1. **Tagged Unions / Interfaces:** Create a generic `column_t` wrapper struct that uses function pointers (vtables) or a tagged union to wrap the specific implementations.
2. **Opaque Pointers + Schema:** Store them as an array of raw `void*` pointers, and rely entirely on the `schema_registry_t` to tell us what they actually are. (e.g., The schema says "Column 1 is a String", so we safely cast `column_buffers[1]` to `string_column_t*`).

For now, we are using the **Opaque Pointers + Schema** approach to keep the struct lightweight. 

*Pedagogical Warning:* Because `void*` completely strips away compile-time type safety, the `schema_registry_t` becomes our *only* safety net. If the parser layer accidentally passes an array of `float`s for a column that the schema defines as `COL_TYPE_I32`, the C compiler cannot save you. The engine will blindly `memcpy` the float bits into the integer buffer, resulting in silently corrupted data. This places a massive architectural responsibility on the upstream Parser layer to strictly honor the schema before calling `table_insert_batch`.

#### The Hidden Cost of Row-by-Row Ingestion

It is tempting to think that we only need to check the schema when we *create* the column. However, because our ingestion function is `table_insert_batch(..., const void** values)`, we have a hidden performance trap.

If we insert data row-by-row, our inner loop looks like this:
```text
Row-by-Row Ingestion (Slow - 100M branches):
For each ROW (10,000,000 iterations):
    For each COLUMN (10 iterations):
        Switch(column.type) ──► CPU branch predictor thrashing!
            Insert single value
```
For 10 million rows with 10 columns, the `switch` statement is evaluated **100 million times** at runtime. In high-performance OLAP databases, this constant branching inside the inner loop ruins CPU branch prediction and destroys performance. This is known as "interpretation overhead." 

To solve this, we perform what is called the **"Loop Flip"**. Instead of ingesting a single row at a time, we ingest a **Batch of Columns**, pushing the column loop to the outside:
```text
Columnar Batch Ingestion (Fast - 10 branches):
For each COLUMN (10 iterations):
    Switch(column.type)     ──► Branch evaluated exactly ONCE per column!
        Bulk copy 10,000,000 values directly into the buffer
```
This allows us to hit the switch statement exactly *once* per column. We can then pass the entire array of thousands of values directly to the underlying columnar storage (which loops over the rows on the inside without any type checking).

#### Bulk Memory Copies (SIMD Efficiency)

If we switch to `table_insert_batch` but still ingest the values one-by-one inside the switch statement (e.g., calling a scalar `buffer_i32_push()` in a `for` loop), we are leaving massive performance on the table. 

Because our incoming batch data is already a contiguous array (e.g., `int32_t*`), and our internal columnar buffer is also a contiguous array, we can skip the loop entirely! We just ensure our buffer has enough capacity, and then execute a single **`memcpy()`**. 

Standard C library implementations of `memcpy()` are hyper-optimized for your specific CPU architecture, heavily utilizing **SIMD vector instructions** (like AVX-512 or ARM Neon) to copy massive blocks of memory at hardware bandwidth limits. 

By pushing the loop down into a single `memcpy()`, the CPU copies thousands of rows in a fraction of a microsecond.

#### Why not Zero-Copy? (Memory Ownership)

If zero-copy is the holy grail of database performance (as we saw with `mmap` earlier), why don't we just assign the pointer directly?
```c
// Why not do this?
table->column_buffers[i] = columnar_values[i]; 
```

The answer comes down to **Memory Ownership and Lifecycles**. 
The `columnar_values[i]` array is provided by the Parser layer. Because allocating memory is slow, the Parser will typically allocate one buffer, fill it with a batch of data, pass it to `table_insert_batch`, and then *reuse that exact same buffer* for the next batch of data. 

If the Table simply zero-copies the pointer, the Parser will immediately overwrite the Table's data on the very next batch! 

Furthermore, the Table's job is to accumulate many small batches into one massive, contiguous block of RAM before flushing to disk. By using `memcpy`, the Table safely copies transient data from the Parser's temporary buffer into its own stable, contiguous accumulation buffer.

### C Quirks: Switch-Case Scoping and Batch Buffer Copies

When implementing `table_insert_batch`, there are four common pitfalls to watch out for:

1. **Switch-Case Variable Declaration Scoping (`-Wc23-extensions`):** In C11 and earlier standards, labels (including `case` labels) can only precede *statements*, not *declarations*. Placed directly after a label without a block, Clang will emit:
   ```text
   warning: label followed by a declaration is a C23 extension [-Wc23-extensions]
   ```
   To remain strictly standard-compliant in C11 without relying on C23 compiler extensions, always enclose the case body in curly braces:
   ```c
   case COL_TYPE_I32: {
       buffer_i32_t* buf = (buffer_i32_t*)table->column_buffers[i];
       // ...
       break;
   }
   ```

2. **Additive Capacity vs. Total Rows:** Calling `ensure_capacity(buf, num_rows)` will truncate or fail to allocate enough space if the buffer already contains existing rows. You must allocate space for the *new total*:
   ```c
   buffer_i32_ensure_capacity(buf, buf->size + num_rows);
   ```

3. **`memcpy` vs. `memccpy`:** `memccpy()` is a string utility designed to stop copying when it encounters a specific character (such as `\n`). For raw numerical arrays, always use `memcpy()`.

4. **Copy Offset and Size Tracking:** The destination of `memcpy` must be offset to where the unwritten space begins (`buf->data + buf->size`), and `buf->size` must be incremented by `num_rows` after the copy. Reassigning local pointers (like `col_ptr = columnar_values[i]`) does not modify `table->column_buffers[i]` and contradicts using `memcpy`.

### The Buffer Overwrite Trap: Offset and Size Tracking

When appending batches to an active in-memory buffer, two critical steps must occur:

1. **The Destination Offset and Pointer Arithmetic Scaling (`buf->data + buf->size`):**
   If you copy directly to `buf->data`, you are always writing to index `0`. 
   - **Batch 1 (1,000 rows):** Writes to indices `0..999`.
   - **Batch 2 (1,000 rows):** Writes to indices `0..999` again, **completely overwriting Batch 1!**
   
   To append data sequentially, the destination pointer must be offset by the existing count. However, this is a classic C pointer arithmetic trap! In C, pointer addition automatically scales by the size of the casted type. 
   
   ```c
   // WRONG: If buf->data is a void* or uint8_t*, adding buf->size (1000) 
   // offsets by 1000 literal bytes. This only leaves room for 250 integers!
   memcpy(buf->data + buf->size, ...);
   
   // RIGHT: Cast the pointer to (int32_t*) BEFORE adding buf->size. 
   // Adding buf->size (1000) now correctly offsets by 1000 * 4 bytes!
   memcpy((int32_t*)buf->data + buf->size, ...);
   ```

2. **Incrementing `buf->size` and `table->row_count`:**
   `memcpy` only moves raw bytes in RAM; it does not update our struct's metadata. If you do not execute `buf->size += num_rows;`, the buffer's size counter remains `0`. Every subsequent insert will believe the buffer is empty and blindly overwrite previous data.

```c
case COL_TYPE_I32: {
    buffer_i32_t* buf = (buffer_i32_t*)table->column_buffers[i];
    buffer_i32_ensure_capacity(buf, buf->size + num_rows);
    // 1. Write starting at the tail offset
    memcpy(buf->data + buf->size, columnar_values[i], num_rows * sizeof(int32_t));
    // 2. Increment the buffer size counter
    buf->size += num_rows;
    break;
}
```

### Anatomy of a Dictionary-Encoded Column: Two Design Options

A dictionary-encoded column requires two distinct pieces of state:
1. **The Dictionary (`string_dict_t*`):** Deduplicates unique strings to integer IDs (`"apple" -> 0`, `"banana" -> 1`).
2. **The Key Array (`buffer_i32_t*`):** Contiguous array of integer surrogate keys for each row (`[0, 1, 0, 0, 1, ...]`).

How should `table_t` represent this in its `column_buffers`?

#### Option A: Dedicated Container Struct (`string_dict_column_t`)
Bundle both objects into a dedicated container struct stored in `column_buffers[i]`:
```c
typedef struct {
    string_dict_t* dict;
    buffer_i32_t*  keys_buf;
} string_dict_column_t;
```

#### Option B: Uniform 32-bit Buffers (Mechanics & Limitations)
In Option B, `table_t` exploits the fact that `I32`, `F32`, and `STRING_DICT` keys all happen to be exactly 4 bytes (32 bits) in size. 
`table_t` maintains two parallel arrays:
```c
typedef struct {
    // ...
    buffer_i32_t** column_buffers; // Array of 32-bit buffers
    string_dict_t** string_dicts;  // NULL for numeric columns, populated for string_dict columns
} table_t;
```
During insertion:
```c
case COL_TYPE_STRING_DICT: {
    string_dict_t* dict = table->string_dicts[i];
    buffer_i32_t*  keys = table->column_buffers[i];
    string_dict_encode_column(dict, (const char**)columnar_values[i], num_rows, keys);
    break;
}
```

#### Why Option A Wins (The Breakdown of Option B)
While Option B looks tempting when all types are 32-bit, it completely breaks down when:
1. **Raw Variable-Length Strings (`COL_TYPE_STRING`):** Uncompressed text cannot fit into a flat `buffer_i32_t*`.
2. **64-bit Types (`I64`, `F64`, `TIMESTAMP`):** 8-byte values require different buffer alignment and stride.

Therefore, **Option A** (polymorphic `void** column_buffers` pointing to dedicated column structs) is the superior, extensible design.

### In-Memory Representation of Raw Strings (`COL_TYPE_STRING`)

Unlike fixed-width integers, raw strings have variable lengths. During ingestion, `columnar_values[i]` is passed as an array of string pointers (`const char**`).

To buffer raw strings in memory before flushing, we define a dynamic string array struct:

```c
typedef struct {
    char** strings;   // Array of dynamically allocated string pointers
    size_t size;      // Current number of strings
    size_t capacity;  // Allocated pointer capacity
} raw_string_column_t;

// Dedicated constructor for clean, zero-initialized lifecycle management
static inline raw_string_column_t* raw_string_column_create(size_t initial_capacity) {
    raw_string_column_t* col = calloc(1, sizeof(raw_string_column_t));
    if (!col) return NULL;
    if (initial_capacity > 0) {
        col->strings = malloc(initial_capacity * sizeof(char*));
        col->capacity = initial_capacity;
    }
    return col;
}

static inline void raw_string_column_free(raw_string_column_t* col) {
    if (!col) return;
    if (col->strings) {
        for (size_t r = 0; r < col->size; r++) {
            free(col->strings[r]);
        }
        free(col->strings);
    }
    free(col);
}
```

#### The Shallow `memcpy` vs. Deep `strdup` Trap
When ingesting an array of pointers (`char**`), using `memcpy(col->strings + col->size, ..., num_rows * sizeof(char*))` only performs a **shallow copy** of the pointer addresses. If the destructor later runs `free(col->strings[r])`, calling `free()` on string literals (from `.rodata`) or stack-allocated buffers will cause an instant fatal `SIGABRT` crash!

To ensure the table possesses genuine, safe ownership of variable-length text data, we **deep-copy** each string with `strdup()` during ingestion:
```c
case COL_TYPE_STRING: {
    raw_string_column_t* col = (raw_string_column_t*)table->column_buffers[i];
    if (col->size + num_rows > col->capacity) {
        size_t new_cap = (col->capacity == 0) ? 128 : col->capacity * 2;
        while (new_cap < col->size + num_rows) new_cap *= 2;
        col->strings = realloc(col->strings, sizeof(char*) * new_cap);
        col->capacity = new_cap;
    }
    for (size_t r = 0; r < num_rows; r++) {
        col->strings[col->size + r] = strdup(((const char **)columnar_values[i])[r]);
    }
    col->size += num_rows;
    break;
}
```

### C Quirks: Struct Initialization and Strings

When implementing the Table creation and destruction lifecycle, there are two classic C memory traps to avoid:

1. **Fixed-Size Arrays vs. Pointers:** In our struct, we defined `char name[64];`. In C, an array name represents a constant pointer to its first element. You cannot use the assignment operator (`=`) to assign a string to it. Instead, you must copy the memory using `strncpy`. 
   
   *Warning:* If you just use `strncpy(table->name, name, 64)`, and the input string is 64 characters or longer, C will fill the entire buffer but will **fail to append a null terminator (`\0`)**, causing string reading functions to crash later. To be safe, always leave room for the terminator and set it explicitly:
   ```c
   strncpy(table->name, name, sizeof(table->name) - 1);
   table->name[63] = '\0'; // Ensure null-termination
   ```

2. **Garbage Memory (`malloc` vs. `calloc`):** If you allocate a struct using `malloc(sizeof(table_t))`, the memory is uninitialized. Fields like `table->row_count`, `table->column_buffers`, and `table->parts` will contain random garbage values, leading to instant segfaults when we try to check if they are NULL later. 
   
   To initialize the buffers manually, you have to explicitly set every pointer to NULL:
   ```c
   table->column_buffers = NULL;
   table->parts = NULL;
   table->schema = NULL;
   ```
   However, the much safer approach is to use `calloc(1, sizeof(table_t))` (or `memset(table, 0, sizeof(*table))`). Because `NULL` is effectively `0` under the hood, `calloc` automatically sets every integer to `0` and every pointer to `NULL` for you in one fell swoop!

### The Destructor Polymorphism Trap

When writing `table_destroy`, it is easy to assume that calling `free(table->column_buffers)` is sufficient. However, because `column_buffers` is an array of `void*` pointers pointing to completely different columnar structures (e.g. `buffer_i32_t*`, `string_dict_column_t*`, `raw_string_column_t*`), just freeing the array leaks all the underlying column memory!

To properly destroy the table, we must perform the "Loop Flip" in reverse:
1. Iterate through `table->schema->columns`.
2. Check the `type` of each column.
3. Cast `table->column_buffers[i]` back to its original type and call its specific destructor.
4. Finally, `free(table->column_buffers)` itself.

```c
if (table->column_buffers && table->schema) {
    for (size_t i = 0; i < table->schema->count; i++) {
        void* col_ptr = table->column_buffers[i];
        if (!col_ptr) continue;
        
        switch (table->schema->columns[i].type) {
            case COL_TYPE_I32:
            case COL_TYPE_F32:
                buffer_i32_free((buffer_i32_t*)col_ptr);
                break;
            case COL_TYPE_STRING_DICT: {
                string_dict_column_t* col = (string_dict_column_t*)col_ptr;
                string_dict_free(col->dict);
                buffer_i32_free(col->keys_buf);
                free(col);
                break;
            }
            case COL_TYPE_STRING:
                raw_string_column_free((raw_string_column_t*)col_ptr);
                break;
        }
    }
    free(table->column_buffers);
}
```

### What About JSON or CSV? (The Parser Layer)

You might be wondering: How do we actually ingest JSON, NDJSON, or CSV files if our `table_insert_batch` function expects strongly-typed C arrays (`const void***`)?

This highlights another critical separation of concerns in database architecture. The `table_t` abstraction is strictly an **internal storage engine coordinator**. It does not know what a JSON string or a CSV comma is. 

Parsing text formats is extremely CPU-intensive and messy. Therefore, databases separate this into a distinct **Format Parser Layer**. 
In a future stage, we will build a parser (e.g., an NDJSON reader) whose sole job is to read raw text, coerce the strings into integers/floats/dictionaries, and build up the `columnar_values` arrays in memory. Once a batch of 8,192 rows is fully parsed and typed, the parser hands that clean memory block down to `table_insert_batch`.

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

## Unit Tests (`tests/test_db.c`)

```c
void test_table_creation(void) {
    table_t* t = table_create("events");
    assert(t != NULL);
    assert(strcmp(t->name, "events") == 0);
    assert(t->row_count == 0);

    table_destroy(t);
    printf("test_table_creation passed.\n");
}

void test_table_e2e_batch_ingestion(void) {
    printf("Testing Table E2E Batch Ingestion...\n");

    // 1. Create table & setup 4-column heterogeneous schema
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
