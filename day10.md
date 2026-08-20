# Day 10: The Sparse Primary Index (2-Tier Query Pruning)

Today, we tackle one of the most  architectural optimizations in OLAP databases: **The Sparse Primary Index**.

In a traditional OLTP database like PostgreSQL or MySQL, primary keys are indexed using a **B-Tree** (often a dense index). In a dense index, every single row in the database has a pointer in the index. If you have 1 billion rows, your index contains 1 billion entries. These dense indexes often consume hundreds of gigabytes of RAM, drastically inflating database operating costs.

But in analytical databases (OLAP), we are usually dealing with  scale and running aggregations across millions of rows. Storing a pointer for every single row is a  waste of memory. 

ClickHouse solved this  with **Sparse Indexing**.

---

## 1. The Sparse Index Concept

Recall from Day 9 that our data is physically streamed into `.bin` files and logically chunked into 8,192-row **Granules**.

**The Golden Rule of OLAP:** The data on disk *must* be physically sorted by the Primary Key. In an analytical database, the term `PRIMARY KEY` does not mean "a unique identifier" (like in Postgres), it literally means **"The Sorting Key"**. 

If the data is  sorted, we do not need an index entry for every single row. Instead, we only need **one index entry per Granule**. Specifically, we store the *minimum* primary key value found at the very start of that Granule (the first row of the block). We call this starting value a **"mark"**.

For example, if Granule 0 starts with `user_id = 10`, and Granule 1 starts with `user_id = 50002`, our marks array is simply `[10, 50002]`. These act as signposts that allow the database to locate any row.

### The "Naive" : Defeating the B-Tree

Conceptually, splitting data into chunks and storing a simple array of marks feels  naive and . So why was it a ?

For over 30 years, the database industry (Oracle, Postgres, MySQL) was  with the **Dense B-Tree**. B-Trees are highly complex: they require node splitting, balancing, and locking for concurrency, and they store a pointer for *every single row*. This was perfect for OLTP workloads (like updating a single bank account), but when Big Data arrived, B-Trees for 100-billion row tables swelled to terabytes, exhausted all server RAM, and crushed performance.

The OLAP  of systems like ClickHouse was having the courage to throw the B-Tree away. By choosing a "naive" array over a complex tree, they created an index that is completely lock-free,  contiguous in memory, and trivial to binary search. In database engineering, the best architectures are often the simplest ones.

### The Math of Memory Savings

If we have 1,000,000,000 (One Billion) rows:
- A Dense B-Tree index (assuming 8 bytes per entry) would consume **8 Gigabytes** of RAM just for the index.
- A Sparse Index with a granularity of 8,192 rows would only need 1,000,000,000 / 8192 = 122,070 entries. At 4 bytes per entry, the entire index consumes **488 Kilobytes**.

This is an **8,192x reduction in memory**. A billion-row index can now fit entirely within a modern CPU's L2 cache!

---

## 2. The 2-Tier Pruner

When a user runs a query like `SELECT * FROM table WHERE user_id = 50000`, we need to find which Granules to read. We do this using a 2-Tier Pruning system.

### Tier 1: Part-Level Pruning
Before we even look at the Granules, we track the absolute `min` and `max` primary key value for the *entire* Part.
If a Part contains `user_id`s from `0` to `40000`, and the query asks for `50000`, we can instantly skip the entire Part (and entirely skip searching its index). 

### Tier 2: Granule-Level Pruning (Binary Search)
If the query falls within the Part's range, we load the `.idx` file (an array of our Granule marks) into memory. Because the marks are sorted, we can perform a highly efficient **Binary Search** over the array to find exactly which Granule contains the target value.

For example, if our `.idx` file contains:
`[0, 81920, 163840, 245760]`

And we search for `user_id = 100000`:
1. Binary search finds that `100000` is greater than `81920` (Granule 1), but less than `163840` (Granule 2).
2. Therefore, `user_id = 100000` *must* live inside Granule 1.
3. The Pruner returns `{ .start_granule = 1, .end_granule = 2 }`.

Instead of scanning the whole `.bin` file, our execution engine will use pure math (from Day 9) to `fseek` directly to Granule 1 and only read those 8,192 rows.

---

## 3. The Bitmap Pruner Pivot

When designing the Pruner interface, it might seem  to return a simple `[start, end)` range. Since the primary key is physically sorted on disk, all matching granules are guaranteed to be contiguous.

However, this design permanently breaks if we ever want to index a column that is *not* the primary key.

If we add a secondary "Min-Max Skipping Index" on a `salary` column (which is not physically sorted), a query for `salary = 5000` might match Granule 0, Granule 5, and Granule 12. 

A Min-Max index stores the minimum and maximum value for every Granule (e.g. Granule 0: `[min: 30k, max: 95k]`). Because the `salary` column isn't sorted, we can't binary search it. Instead, the database quickly scans the Min-Max metadata in RAM, checking if the target `5000` falls between the `min` and `max` of each granule, skipping those where it doesn't. 

Since the matches are not contiguous, we cannot represent `[0, 5, 12]` with a simple `[start, end)` struct.

**The Solution: Granule Bitmaps**
Instead of returning a range, the Pruner should return a **Bitmap** where each bit represents a Granule (1 = read it, 0 = skip it). 
If a Part has 200 Granules, the bitmap is only 200 bits (25 bytes). This not only solves the secondary index problem, but it sets up a  foundation for the future: we can search the Primary Index (via binary search), search a Secondary Min-Max Index (via fast RAM sequential scan), and simply do a bitwise `AND` on their two bitmaps to find the exact intersection of Granules we need to read from disk!

---

## 4. Zone Maps & Statistical Indexes (Beyond Min/Max)

If a Min/Max index tells us *which* granules to skip, could we store other metadata to skip reading the `.bin` file entirely?

What if we also stored the `sum`, `count`, `avg`, or `cardinality` (number of unique values) for every Granule?

This is exactly how modern OLAP systems achieve instantaneous aggregations. These are often called **Zone Maps** or **Statistical Projections**.

Imagine a query: `SELECT SUM(salary) FROM employees WHERE department_id = 5;`

If our Primary Index tells us that department 5  spans Granules 10 through 15, we don't need to read the 49,152 rows from the `salary.bin` file! If we have a statistical index that stores the `sum` of salary for every Granule, our execution engine can simply grab the pre-calculated `sum` for Granules 10 through 15 directly from the index metadata. 

By storing aggregates at the Granule level:
1. **Min/Max/Bloom Filters** are used to *filter* (Data Skipping).
2. **Sum/Avg/Count/Cardinality** are used to *aggregate* (Pre-calculation).

This allows aggregation queries over billions of rows to execute in milliseconds because they only read the tiny metadata files, completely bypassing the  `.bin` files!

---

## 5. How the Giants Do It: Indexes in ClickHouse & DuckDB

To understand how our engine fits into the broader database ecosystem, here is a quick look at the types of indexes used by the industry leaders.

### ClickHouse
ClickHouse gives you explicit, granular control over almost every type of index imaginable.
1. **Primary Index**: The exact Sparse Index we are building today. It stores one mark per granule based on the sorting key.
2. **Data Skipping Indexes (Secondary)**: 
  , **`minmax`**: Stores the absolute minimum and maximum values for a set of granules (perfect for timestamps).
  , **`set`**: Stores all unique values within a granule (up to a limit). If your query `WHERE` clause isn't in the set, it skips the granule.
  , **`bloom_filter`**: Probabilistically determines if a value exists in a granule using bit arrays.
  , **`tokenbf_v1` / `ngrambf_v1`**: Specialized bloom filters for  fast text search on  string columns.
3. **Projections**: Allows you to store pre-aggregated statistics (like `SUM` or `AVG`) or even keep a secondary, fully sorted copy of a column (to avoid full table scans when querying by a non-primary key).

### DuckDB
DuckDB takes a "batteries included, zero configuration" approach. It manages indexes completely automatically under the hood so the user doesn't have to think about them.
1. **Zone Maps (Min-Max)**: DuckDB automatically maintains min-max indexes for *every single column* in a table at the Block level (their version of a Granule). This guarantees baseline data skipping for all queries out of the box.
2. **ART (Adaptive Radix Tree)**: DuckDB uses a dense, highly optimized Radix Tree for enforcing `PRIMARY KEY` and `UNIQUE` constraints. If you query for a specific ID, DuckDB uses the ART for  point lookups.
3. **HNSW (Hierarchical Navigable Small World)**: DuckDB extensions support vector indexing to power AI and semantic search workloads natively.

---

## 6. Modifying `storage.h` for Indexing (Fixing the Leaky Abstraction)

Initially, it might seem easiest to directly embed index fields (`idx_fp`, `part_min_val`) into the `column_writer_t`. However, **this is a leaky abstraction**. If we later want to add a Bloom Filter, a Zone Map (sum/avg), or an HNSW vector index, `column_writer_t` would bloat with dozens of specific file pointers and state variables.

To build a truly extensible database, we need to decouple the Column Writer from the Index Writers using a plugin-style architecture (interface callbacks).

We can define a generic `index_writer_t` interface:

```c
typedef struct index_writer_t index_writer_t;

struct index_writer_t {
    // Function pointer to feed data into the index
    void (*append_fn)(index_writer_t* self, const void* data, size_t num_rows, uint64_t current_row_offset);
    
    // Function pointer to finalize and close the index files
    void (*close_fn)(index_writer_t* self);
    
    // Opaque pointer to the specific index's internal state
    void* state; 
};
```

*(**C-Language Tip:** Why do we split the `typedef` and the `struct` definition? Because the function pointers inside the struct take an `index_writer_t* self` argument. If we didn't forward-declare the typedef on the first line, the compiler wouldn't know what `index_writer_t` was when it reached the `append_fn` signature!)*

Then, we modify `column_writer_t` so it simply holds an array of these generic index writers. To keep memory management simple and avoid `realloc`, we can use a small fixed-size array (a column rarely has more than 4 or 5 indexes attached to it):

```c
#define MAX_INDEXES_PER_COLUMN 8

typedef struct {
    const char* column_name;
    column_type_t type;
    FILE* bin_fp;
    uint64_t rows_written;
    
    // Array of attached indexes (Sparse Index, Bloom Filter, etc.)
    index_writer_t* indexes[MAX_INDEXES_PER_COLUMN];
    size_t num_indexes;
} column_writer_t;

// New API function to inject an index from the outside
void column_writer_attach_index(column_writer_t* writer, index_writer_t* index);
```

Now, `column_writer_append` becomes ly clean and agnostic:

```c
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows) {
    // 1. Write raw data to .bin file
    fwrite(data, ...);
    
    // 2. Feed the new chunk to all attached indexes
    for (size_t i = 0; i < writer->num_indexes; i++) {
        writer->indexes[i]->append_fn(writer->indexes[i], data, num_rows, writer->rows_written);
    }
    
    writer->rows_written += num_rows;
}
```

### Implementing a Concrete Index Plugin

C lacks native object-oriented features such as classes or inheritance. To implement polymorphism, an interface pattern is simulated using a struct containing function pointers (`index_writer_t`). Private state variables are encapsulated using an opaque `void* state` pointer. This design pattern allows different index types to be treated uniformly by the column writer.

To make everything play together, we follow three steps:

**1. The Private State**
We define a private struct to hold the data specific to our Sparse Index:
```c
typedef struct {
    FILE* idx_fp;
    int32_t min_val;
    int32_t max_val;
    size_t num_marks;
} sparse_index_writer_state_t;
```

**2. The Concrete Functions**
We write `static` C functions that match the signatures required by the interface. Inside, we cast the opaque `self->state` pointer back to our private struct:
```c
static void sparse_index_writer_append(index_writer_t* self, const void* data, size_t num_rows, uint64_t current_row_offset) {
    sparse_index_writer_state_t* state = (sparse_index_writer_state_t*)self->state;
    // ... write to state->idx_fp ...
}

static void sparse_index_writer_close(index_writer_t* self) {
    sparse_index_writer_state_t* state = (sparse_index_writer_state_t*)self->state;
    // ... write the sparse_index_file_header_t to the top of the file, then fclose ...
}
```

**3. The Wiring (Factory)**
Finally, the factory function allocates the interface, allocates the state, and wires the pointers together:
```c
index_writer_t* sparse_index_writer_create(const char* part_dir, const char* col_name) {
    // 1. Allocate the generic interface
    index_writer_t* writer = malloc(sizeof(index_writer_t));
    
    // 2. Allocate the private state and open the file
    sparse_index_writer_state_t* state = malloc(sizeof(sparse_index_writer_state_t));
    // ... construct path and state->idx_fp = fopen(...) ...
    
    // 3. Wire it all together!
    writer->state = state;
    writer->append_fn = sparse_index_writer_append;
    writer->close_fn = sparse_index_writer_close;
    
    return writer;
}
```

The database ingestion loop simply calls `sparse_index_writer_create()`, gets the generic `index_writer_t*`, and injects it into the primary key's column writer!

With this architecture, `column_writer_t` has no idea what a Sparse Index or a Bloom Filter is. It just streams bytes to disk and notifies its plugins, completely fixing the leaky abstraction!

---

## 7. Bridging Day 9 and Day 10: Implementing `storage.c`

Before we build the Sparse Index in memory, we must ensure our engine can physically write the `.bin` files that the index will point to. We will implement the `column_writer_t` we designed in Day 9.

When writing `column_writer_create(part_dir, col_name, type)`:
1. **Safe Paths**: We use `snprintf` to combine `part_dir` and `col_name` into a local buffer, preventing buffer overflows.
2. **Binary Mode**: We must use `fopen(path, "wb")` because we are writing raw binary chunks (not text).
3. **Graceful Failures**: If `fopen` returns `NULL` (e.g., directory missing or permission denied), we must gracefully clean up any allocated memory and return `NULL` to the caller, rather than panicking with `assert()`. This ensures the database daemon stays alive even if a specific query encounters an I/O error.

Once the `column_writer_append` logic is streaming chunks of 8,192 rows into these `.bin` files, our Sparse Index will be able to  locate them.

---

## 8. The Headers (Read-Path vs Write-Path)

In our database, it is  to separate how data is ingested from how it is queried.
- **The Write-Path**: In `storage.h`, we defined the generic `index_writer_t` interface. This is what the ingestion engine uses to physically build the `.idx` files as data streams in.
- **The Read-Path**: When a query executes, we don't use the writer. Instead, the execution engine loads the pre-built index into RAM using a highly optimized read-only struct.

With the Bitmap strategy in mind, we can draft the Read-Path data structures in `include/simpledb/sparse_index.h`:

```c
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "simpledb/bitmap.h"
#include "simpledb/storage.h"

// The physical header written at the very beginning of every .idx file
typedef struct {
    uint32_t ;      // e.g., 0x5052494D ('P' 'R' 'I' 'M')
    uint32_t version;    // e.g., 1
    size_t num_marks;    // Number of 32-bit marks that follow this header
    int32_t min_val;     // Absolute minimum value for the entire part
    int32_t max_val;     // Absolute maximum value for the entire part
} sparse_index_file_header_t;

// The in-memory representation of the primary.idx file
typedef struct {
    sparse_index_file_header_t header; // Embedded physical metadata
    int32_t* marks;                    // Array of primary key values, one per granule
} sparse_index_t;

// --- Read-Path ---
// Load a sparse index from disk
sparse_index_t* sparse_index_load(const char* idx_file_path);

// Free the index
void sparse_index_free(sparse_index_t* idx);

// 2-Tier Pruning logic
bool sparse_index_search(const sparse_index_t* idx, int32_t target_val, bitmap_t* out_mask);

// --- Write-Path Factory ---
// Creates the concrete index plugin used by storage.c to write the index
struct index_writer_t* sparse_index_writer_create(const char* part_dir, const char* col_name);
```

### The Lifecycle of the Index: Disk vs Memory

Looking at the `sparse_index_load` function, a critical question arises: *Does this function scan the entire `.bin` file to build the index in memory?*

**Absolutely not.** If we had to scan the  `.bin` file every time we ran a query, it would defeat the entire purpose of the database!

The Sparse Primary Index has two distinct phases:

1. **Write Time (Persistence):** When the database ingests data and flushes a new Part to disk, it creates the  `column.bin` files. But simultaneously, it also writes a tiny `primary.idx` file to the same directory. This `.idx` file contains the raw binary array of the minimum values for each granule. The index is fully calculated once and **persisted to disk forever**.
2. **Read Time (Loading):** When a user runs a query, the execution engine calls `sparse_index_load`. This function does not touch the `.bin` files at all. It simply opens the tiny, pre-calculated `primary.idx` file and loads its contents directly into the `sparse_index_t` struct in RAM. Because the file is  small (e.g., 488 KB for a billion rows), this loading process is nearly instantaneous.

The `sparse_index_t` struct is strictly the **in-memory representation** of the index, kept in RAM to allow  binary searches during query execution.

---

## 9. The Implementation Guide (`sparse_index.c`)

To bring this all to life, we implement both the Write-Path and Read-Path in `src/sparse_index.c`. Here is exactly what each function is responsible for:

### The Write-Path (Building the Index)
* **`sparse_index_writer_create`**: The factory. It uses `snprintf` to build the `part_dir/col_name.idx` path, opens the file with `fopen(..., "wb")`, and allocates the private state. ly, it must leave a 24-byte "hole" at the start of the file (using `fseek(fp, sizeof(sparse_index_file_header_t), SEEK_SET)`) so we can write the header later! It initializes `min_val` to INT32_MAX, `max_val` to INT32_MIN, and `num_marks` to 0.
* **`sparse_index_writer_append`**: The data feeder. Because `data` is a void pointer, we cast it to `const int32_t*`. Because the Primary Key is physically sorted, we don't need a loop or SIMD to find the min/max! The absolute minimum is simply `values[0]` (on the very first chunk), and the maximum is always `values[num_rows, 1]`. To extract the marks, we mathematically calculate the exact array indices that land on an 8,192 boundary and jump directly to them.
* **`sparse_index_writer_close`**: The finalizer. It builds the 24-byte `sparse_index_file_header_t` using the final min/max/count. It uses `fseek(fp, 0, SEEK_SET)` to jump back to the very beginning of the file, writes the header into the "hole", and closes the file.

### The Read-Path (Querying the Index)
* **`sparse_index_load`**: The reader. It opens the `.idx` file, uses `fread` to pull the first 24 bytes directly into `idx->header`, uses `header.num_marks` to `malloc` the exact size needed for the `marks` array, and `fread`s the rest of the file into that array.
* **`sparse_index_search`**: The 2-Tier Pruner. The query engine passes in an empty `bitmap_t` mask where every bit represents a Granule on disk.
 , **Tier 1 (The Fast-Fail):** Checks if `target_val` is completely outside `header.min_val` and `header.max_val`. If so, returns `false` instantly. The query engine sees `false` and skips reading the entire file.
 , **Tier 2 (Binary Search):** If within bounds, it performs a Binary Search over the `marks` array in RAM to find the exact Granule where `target_val` resides (the largest mark `<= target_val`). 
 , **The Output:** Once the exact Granule is found (e.g., index `i`), it calls `bitmap_set(out_mask, i)` to flip that specific bit to `1` and returns `true`. The query engine will later inspect this mask, see the `1`, and mathematically `fseek` directly to Granule `i` in the `.bin` file, skipping everything else!
* **`sparse_index_free`**: Simply frees `idx->marks` and the `idx` struct itself.

### Common Pitfalls when Implementing the Read-Path
When implementing the `sparse_index_search` and `sparse_index_load` functions, there are three catastrophic edge cases you must avoid:
1. **The Double Pointer Overwrite:** In `sparse_index_load`, `idx->marks` is already a pointer (allocated via `malloc`). Calling `fread(&idx->marks, ...)` passes the address of the pointer itself, causing the OS to overwrite the pointer with integer data, resulting in an instant segfault. Always use `fread(idx->marks, ...)`.
2. **The Exact Match Bug:** If you attempt a linear scan like `while (marks[i] < target)`, a search for an exact Granule boundary (e.g., searching for `50000` when `marks[1] == 50000`) will fail the `<` check and incorrectly assign the target to the previous Granule.
3. **Unsigned Integer Underflow:** If you attempt to fix the linear scan by subtracting 1 (`i, 1`), a search for a value smaller than `marks[0]` will cause `0, 1`. Since `size_t` is unsigned, this underflows to `18,446,744,073,709,551,615`, crashing the bitmap setter.
4. **Using C's built-in `bsearch`:** It might be tempting to include `<stdlib.h>` and use C's built-in `bsearch()` function instead of writing your own loop. You **cannot** do this! C's `bsearch` only finds *exact matches*. If a Granule starts at `50000` and the next starts at `100000`, a search for `60000` will cause `bsearch` to return `NULL`. We don't want an exact match; we want the bounding Granule (the largest mark `<= target_val`).

**The Solution:** Always use a custom `O(log N)` Binary Search loop designed to find the *largest index where `marks[mid] <= target_val`*. C doesn't have C++'s `std::upper_bound`, so we must write this search manually. This handles all exact matches, prevents underflows, and correctly bounds values.

## 10. Testing the Bitmap Pruner

Before we write the implementation, let's look at how we will test it. We will write a unit test in `tests/test_db.c` that mocks a Sparse Index in memory and queries it.

Because our Pruner now returns a Bitmap instead of a single range, we will use our `bitmap_t` API (from Day 6) to verify that exactly the right Granule bits are flipped to `1`.

```c
void test_sparse_index_search(void) {
    // 1. Mock a Sparse Index with 4 Granules
    // Granule 0 starts at 0, Granule 1 starts at 1000, etc.
    int32_t mock_marks[] = {0, 1000, 2000, 3000};
    
    sparse_index_t idx;
    idx.marks = mock_marks;
    idx.num_marks = 4;
    idx.min_val = 0;
    idx.max_val = 3999; // Assume the max value in the part is 3999
    
    // 2. Create a Bitmap mask to hold the results (4 granules = 1 byte needed)
    bitmap_t* mask = bitmap_create(1);
    
    // 3. Search for a value that should fall squarely in Granule 1 (1000 to 1999)
    bool found = sparse_index_search(&idx, 1500, mask);
    
    // 4. Assertions
    assert(found == true);
    
    // Granule 1's bit should be flipped to 1 (true)
    assert(bitmap_is_set(mask, 1) == true);
    
    // All other granules should be 0 (false)
    assert(bitmap_is_set(mask, 0) == false);
    assert(bitmap_is_set(mask, 2) == false);
    assert(bitmap_is_set(mask, 3) == false);
    
    bitmap_free(mask);
}
```

This test  validates the 2-Tier Pruning logic. If we searched for `5000` (which is outside `idx.max_val`), the Pruner's Tier-1 check would immediately return `false`, allowing the execution engine to skip reading the `.bin` file entirely without even doing a binary search!

### Going Further: The End-to-End Integration Test

While unit testing the math is great, you are completely right, it doesn't test the physical storage, the object-oriented factory, or the dynamic loading!

To truly test our pipeline, we will write a second, full End-to-End Integration test in `tests/test_db.c`:

1. **Write Phase**: Use `sparse_index_writer_create("/tmp", "user_id")` to instantiate the plugin.
2. **Append Phase**: Mock a loop that generates 40,000 sorted integers, and feed them into `writer->append_fn()` chunk-by-chunk. This forces the plugin to calculate the boundaries, extract the marks, and stream them to the temporary `.idx` file.
3. **Close Phase**: Call `writer->close_fn()` to seek back to byte 0 and flush the `sparse_index_file_header_t` to disk.
4. **Load Phase**: Use `sparse_index_load("/tmp/user_id.idx")` to parse the file back from disk into the `sparse_index_t` RAM struct.
5. **Search Phase**: Perform the `sparse_index_search` against the loaded struct.
6. **Cleanup**: Free the memory and delete the temp file.

Here is what that full test looks like in C:

```c
void test_sparse_index_e2e(void) {
    const char* tmp_dir = "/tmp";
    const char* col_name = "test_col";
    
    // 1. Create the writer
    index_writer_t* writer = sparse_index_writer_create(tmp_dir, col_name);
    assert(writer != NULL);
    
    // 2. Append 24,576 rows (exactly 3 Granules)
    int32_t chunk[8192];
    uint64_t row_offset = 0;
    
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 8192; j++) {
            chunk[j] = row_offset + j;
        }
        // Feed chunk into the index
        writer->append_fn(writer, chunk, 8192, row_offset);
        row_offset += 8192;
    }
    
    // 3. Finalize and close the file (writes the header!)
    writer->close_fn(writer);
    free(writer); // The factory malloc'd the index_writer_t struct
    
    // 4. Read Phase: Load the index back from disk
    char idx_path[256];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", tmp_dir, col_name);
    
    sparse_index_t* idx = sparse_index_load(idx_path);
    assert(idx != NULL);
    assert(idx->header.num_marks == 3);
    assert(idx->header.min_val == 0);
    assert(idx->header.max_val == 24575);
    
    // 5. Search Phase
    bitmap_t* mask = bitmap_create(1); // 1 byte holds 8 bits
    
    // Search for 10000, which falls in Granule 1 (8192 to 16383)
    bool found = sparse_index_search(idx, 10000, mask);
    assert(found == true);
    assert(bitmap_is_set(mask, 1) == true);
    assert(bitmap_is_set(mask, 0) == false);
    
    // 6. Cleanup
    bitmap_free(mask);
    sparse_index_free(idx);
    remove(idx_path); // Clean up the temp file
}
```

This E2E test guarantees that our C binary serialization logic (`fwrite`/`fread`) exactly matches our struct definitions, preventing any alignment or padding bugs from sneaking into production!

---

## 11. The Final Implementation (sparse_index.c)

Since this was a highly educational exercise, here is the final, completely working implementation of the Sparse Index that we built together, including the fast-fail bounds checks, the O(log N) binary search, and the zero-copy file header formatting.

```c
#include "simpledb/sparse_index.h"
#include "simpledb/bitmap.h"
#include "simpledb/storage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    FILE *idx_fp;
    int32_t min_val;
    int32_t max_val;
    size_t num_marks;
} sparse_index_writer_state_t;

static void sparse_index_writer_append(index_writer_t *self, const void *data, size_t num_rows,
                                       uint64_t current_row_offset) {
    sparse_index_writer_state_t *state = (sparse_index_writer_state_t *)self->state;
    const int32_t *values = (const int32_t *)data;
    if (num_rows == 0)
        return;

    // Since the data is sorted
    if (state->num_marks == 0 && current_row_offset == 0) {
        state->min_val = values[0];
    }
    state->max_val = values[num_rows, 1];

    // We can do this because data has constant size (i32), once it will become compressed or
    // we hold strings we will need to create a granule map
    size_t i = 0;
    if (current_row_offset % 8192 != 0) {
        i = 8192, (current_row_offset % 8192); // Jump to the first boundary in this chunk
    }

    // since data is sorted
    while (i < num_rows) {
        fwrite(&values[i], sizeof(int32_t), 1, state->idx_fp);
        state->num_marks++;
        i += 8192;
    }
}

static void sparse_index_writer_close(index_writer_t *self) {
    sparse_index_writer_state_t *state = (sparse_index_writer_state_t *)self->state;
    sparse_index_file_header_t header = {.max_val = state->max_val,
                                         .min_val = state->min_val,
                                         .num_marks = state->num_marks,
                                         .magic = 0x5052494D,
                                         .version = 1};
    fseek(state->idx_fp, 0, SEEK_SET);
    fwrite(&header, sizeof(sparse_index_file_header_t), 1, state->idx_fp);

    fclose(state->idx_fp);
}

struct index_writer_t *sparse_index_writer_create(const char *part_dir, const char *col_name) {
    index_writer_t *writer = malloc(sizeof(index_writer_t));
    sparse_index_writer_state_t *state = malloc(sizeof(sparse_index_writer_state_t));
    state->max_val = 0;
    state->min_val = 0;
    state->num_marks = 0;

    char idx_file_path[1024];
    snprintf(idx_file_path, sizeof(idx_file_path), "%s/%s.idx", part_dir, col_name);

    state->idx_fp = fopen(idx_file_path, "wb");
    if (!state->idx_fp) {
        free(writer);
        free(state);
        return NULL;
    }
    fseek(state->idx_fp, sizeof(sparse_index_file_header_t), SEEK_SET);

    writer->state = state;
    writer->append_fn = sparse_index_writer_append;
    writer->close_fn = sparse_index_writer_close;

    return writer;
}

void sparse_index_free(sparse_index_t *idx) {
    free(idx->marks);
    free(idx);
}

bool sparse_index_search(const sparse_index_t *idx, int32_t target_val, bitmap_t *out_mask) {

    if (idx->header.num_marks == 0) {
        return false;
    }

    if (target_val < idx->header.min_val || target_val > idx->header.max_val) {
        return false;
    }

    size_t low = 0;
    size_t high = idx->header.num_marks - 1;
    size_t match_idx = 0;

    while (low <= high) {
        size_t mid = low + (high - low) / 2;

        if (idx->marks[mid] <= target_val) {
            match_idx = mid; // This is a valid candidate
            low = mid + 1;   // But check if there is a larger valid one
        } else {
            high = mid - 1;  // This mark is too big
        }
    }

    bitmap_set(out_mask, match_idx);
    return true;
}

sparse_index_t *sparse_index_load(const char *idx_file_path) {
    sparse_index_t *idx = malloc(sizeof(sparse_index_t));
    FILE *fp = fopen(idx_file_path, "rb");
    if (!fp) {
        free(idx);
        return NULL;
    }

    fread(&idx->header, sizeof(sparse_index_file_header_t), 1, fp);
    if (idx->header.magic != 0x5052494D || idx->header.version != 1) {
        fclose(fp);
        free(idx);
        return NULL;
    }

    if (idx->header.num_marks > 0) {
        idx->marks = malloc(sizeof(int32_t) * idx->header.num_marks);
        if (!idx->marks) {
            fclose(fp);
            free(idx);
            return NULL;
        }
        fread(idx->marks, sizeof(int32_t), idx->header.num_marks, fp);
    } else {
        idx->marks = NULL;
    }

    fclose(fp);
    return idx;
}
```

## 12. The Final Implementation (storage.c)

You also noticed that Day 10 required us to modify `storage.c` to add the plugin architecture (so `column_writer_t` could notify the index writers). Here is the final implementation of the `column_writer` bridging Day 9 and Day 10!

```c
#include "simpledb/storage.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

column_writer_t *column_writer_create(const char *part_dir, const char *col_name,
                                      column_type_t type) {

    column_writer_t *writer = malloc(sizeof(column_writer_t));
    if (!writer) return NULL;

    char bin_file_path[1024];
    snprintf(bin_file_path, sizeof(bin_file_path), "%s/%s.bin", part_dir, col_name);

    writer->bin_fp = fopen(bin_file_path, "wb");
    if (!writer->bin_fp) {
        free(writer);
        return NULL;
    }

    writer->column_name = col_name;
    writer->type = type;
    writer->rows_written = 0;

    // Day 10: Initialize the index array to 0
    writer->num_indexes = 0;

    return writer;
}

void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows) {
    if (num_rows == 0) return;

    // For now we assume all types (I32, F32, STRING_DICT) are 4 bytes.
    // In the future this should be derived from writer->type.
    size_t element_size = 4;
    size_t bytes_to_write = num_rows * element_size;

    // Write the raw bytes to the .bin file
    fwrite(data, 1, bytes_to_write, writer->bin_fp);

    // Day 10: Broadcast the chunk to all attached Index plugins!
    for (size_t i = 0; i < writer->num_indexes; i++) {
        writer->indexes[i]->append_fn(writer->indexes[i], data, num_rows, writer->rows_written);
    }

    writer->rows_written += num_rows;
}

void column_writer_close(column_writer_t* writer) {
    if (writer) {
        if (writer->bin_fp) {
            fclose(writer->bin_fp);
        }
        free(writer);
    }
}

// Day 10: The API to attach a new index plugin
void column_writer_attach_index(column_writer_t* writer, index_writer_t* index) {
    if (writer->num_indexes >= MAX_INDEXES_PER_COLUMN) {
        fprintf(stderr, "[FATAL ERROR] Column has too many indexes\n");
        exit(EXIT_FAILURE);
    }
    writer->indexes[writer->num_indexes++] = index;
}
```
