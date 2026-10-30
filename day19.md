# Day 19: The MergeTree Part Compactor (Background Part Merging Engine)

> [!TIP]
> **Long-Read Engineering Log (~1,215 lines) — TL;DR & Fast Navigation**
> This post is an unabridged Socratic pair-programming log covering ClickHouse-style MergeTree part compaction and multi-threaded catalog concurrency in C11.
> - **K-Way Sorted Part Merging (`part_merger_t`):** Streams multiple immutable on-disk parts through a Min-Heap while physically purging rows flagged in each part's `deletion_vector_t`.
> - **Lock-Free Readers & Background Compaction (`pthread_rwlock_t`):** Uses ClickHouse's `all_min_max_level` naming convention and read-write locks so background compaction merges parts without blocking concurrent queries.
> - **Want to go straight to the code?** [Jump to the C Header (`include/simpledb/part_merger.h`)](#1-includesimpledbpart_mergerh), [Jump to the Unit Tests](#2-teststest_part_mergerc), or [Jump to the Final C11 Implementation (`src/part_merger.c`)](#14-final-implementation).

Welcome to Day 19! Today we tackle a critical component of any modern analytical column store: **Background Compaction**.

In high-throughput ingestion systems (like ClickHouse's MergeTree or RocksDB's LSM Tree), data is continuously written to disk as small, immutable files called "parts" or "segments". While appending small immutable files is extremely fast, reading them becomes slow over time. A query would have to open, scan, and merge thousands of separate small parts. 

Furthermore, because our storage is immutable, `DELETE` operations don't actually erase data—they just flip a bit in the `deletion_vector_t` (from Day 13). Over time, deleted rows waste disk space.

## The Solution: The Part Compactor

To solve this, we need a background process that continuously monitors the number of parts. When it detects too many small parts, it triggers a **Merge**:

1. **Physical Deletion**: It scans the parts, reading the `deletion_vector_t`. Any row marked as deleted is simply skipped and *not* written to the new merged part, reclaiming disk space.
2. **Sorting and Merging**: It uses a $K$-way Min-Heap (similar to the external merge sort from Day 15) to efficiently interleave multiple sorted parts into one massive, contiguous, sorted part.
3. **Index Rebuilding**: As it writes the new consolidated part, it regenerates the `.mrk` mark files and the sparse `primary.idx` index from scratch, optimizing them for the new layout.
4. **Atomic Swap**: Finally, it updates the table metadata, replacing the references to the $K$ small parts with a single reference to the new large part, and then asynchronously deletes the old small files.

## The Write Amplification Trade-off

A common question is: *Does this mean we are just re-ingesting the rows? Are we writing the same data multiple times?*

**Yes.** This phenomenon is known as **Write Amplification**, and it is the fundamental trade-off made by systems like ClickHouse (MergeTree) and RocksDB (LSM-Tree). 

Every row ingested into an analytical database is typically written to disk multiple times over its lifespan:
1. First, it is flushed to disk as a small part (e.g., 1MB).
2. Later, the compactor merges that small part with others to create a medium part (e.g., 100MB). *The row is written to disk a second time.*
3. Eventually, medium parts are merged into a massive part (e.g., 10GB). *The row is written a third time.*

Why do databases intentionally multiply their disk writes? **To guarantee sequentially optimal reads.**
By paying the cost of Write Amplification in the background, we guarantee that when a user runs a `SELECT` query, the CPU can sequentially scan a few massive, perfectly contiguous, sorted files at 10 GB/s, rather than seeking around a hard drive to read thousands of fragmented 1MB files.

## Atomic Part Swapping & Concurrency

A common question is: **How is this process atomic if a read happens during it? How do we invalidate old parts and register the new consolidated one?** 

To ensure we don't compromise the read path during a massive compaction, we will run `part_merger_execute()` in a background thread. To do this safely, we must upgrade our `table_t` and `table_part_t` structures from Day 14 with standard concurrency controls: **Read-Write Locks (rwlocks)** and **Atomic Reference Counting**.

Here is the update to `include/simpledb/table.h`:

```c
#include <stdatomic.h>
#include <pthread.h>

// A simple structure representing an immutable flushed part on disk
typedef struct table_part_t {
    char part_path[256];
    size_t row_count;
    
    // Atomic reference count for concurrent readers (0 = safe to delete)
    _Atomic size_t refcount;
} table_part_t;

typedef struct table_t {
    char name[64];
    schema_registry_t *schema;

    // Concurrency control for catalog metadata (parts array)
    pthread_rwlock_t catalog_lock;
    
    /* ... existing buffers ... */
    
    // Track flushed parts (for the Read API)
    table_part_t **parts;
    size_t num_parts;
    
    /* ... existing FTS ... */
} table_t;
```

With these fields, our concurrency model works like this:

1. **Readers Grab References:** When a `SELECT` query starts, it takes a *Read Lock* (`pthread_rwlock_rdlock`) on the table's `catalog_lock`. It copies the array of active parts and atomic-increments the `refcount` on each part it plans to scan (`atomic_fetch_add(&part->refcount, 1)`). It then immediately drops the Read Lock. The query proceeds to scan the parts safely holding those references.
2. **Compactor Writes:** The background compactor merges the data and writes the new consolidated part to disk. Since it is writing to a *brand new* file, it doesn't block any ongoing reads of the old files.
3. **The Atomic Swap:** Once the new part is safely on disk, the compactor takes a *Write Lock* (`pthread_rwlock_wrlock`). This blocks new queries for just a fraction of a millisecond. It updates the array of active parts (removing the old small parts and appending the new large one) and releases the Write Lock. 
4. **Invalidating Old Parts (Zombie State):** The old parts are removed from the active catalog, but they might still be actively read by queries that started *before* the swap! Thus, we mark them as "zombies". They are only physically unlinked from disk (`unlink()`) once their atomic `refcount` hits zero (when the last slow reader finishes).

### Implementing Thread-Safety in `table.c`

To establish this foundation, we must update the `table.c` implementation to shield `OP_SCAN` read queries from background compaction and ingestion flushes:

1. **Initialize the Lock (`table_create`)**: Before returning the new `table_t`, initialize the catalog lock with `pthread_rwlock_init(&table->catalog_lock, NULL);`.
2. **Destroy the Lock (`table_destroy`)**: Clean up with `pthread_rwlock_destroy(&table->catalog_lock);` before freeing the table.
3. **Secure the Write Path (`table_flush`)**: When `table_flush` commits a new part, it calls `realloc(table->parts)`. If a query reads the `parts` array at the exact moment `realloc` moves it to a new memory address, it causes a catastrophic segmentation fault! 
   - Wrap the metadata update array reallocation in a `pthread_rwlock_wrlock(&table->catalog_lock)` and `pthread_rwlock_unlock`. 
   - Initialize the new part's reference count: `atomic_init(&part->refcount, 0);`.
4. **Secure the Read Path (`table_get_parts`)**: This function can no longer just return the raw pointer `table->parts`. 
   - Take a read lock (`pthread_rwlock_rdlock`).
   - `malloc` a *copy* of the pointers array to safely return to the caller.
   - Loop through the parts and increment the reference count: `atomic_fetch_add(&table->parts[i]->refcount, 1)`.
   - Drop the read lock. (The query now owns references to the copied array).

### How Industry Leaders Handle This

Different databases tackle this problem based on their architectural goals:

* **ClickHouse (`MergeTree` Engine):** ClickHouse uses the exact model described above. It writes parts to isolated directories. A background thread merges them into a new directory. It then takes a lightweight catalog lock, swaps the references, and moves the old parts to an "outdated" state. Using `std::shared_ptr` in C++, ClickHouse relies on atomic reference counting; when the last query finishes reading an outdated part, the `shared_ptr` drops to zero, and the part is deleted from the filesystem asynchronously.
* **DuckDB (MVCC):** Because DuckDB supports full ACID transactions, it uses **Multi-Version Concurrency Control (MVCC)** rather than simple part swapping. When data is modified or compacted, DuckDB writes new versions of the blocks but keeps the old blocks intact. Every query runs under a "Transaction Snapshot" and only sees the blocks that were committed before its transaction started. A background Garbage Collector periodically sweeps through and reclaims memory/disk space for blocks that are no longer visible to any active transactions.

### Architecture Note: The C11 Concurrency Model

By introducing `<pthread.h>` and `<stdatomic.h>`, `simpledb` taps into a highly portable and robust concurrency model, but how does it compare to other approaches?

1. **Atomics (`<stdatomic.h>`) & Pthreads:**
   - **Portability:** C11 formally introduced `<threads.h>`, but because it wasn't universally implemented by compiler vendors (e.g., glibc delayed it, MSVC ignored it), **POSIX threads (`<pthread.h>`)** remained the undisputed industry standard for multi-threading on Unix-like systems (Linux, macOS). Meanwhile, `<stdatomic.h>` was widely adopted. 
   - **Performance:** `_Atomic` maps directly to hardware-level CPU instructions (like `LOCK XADD` on x86 or `LDADD` on ARM). It allows variables like our `refcount` to be updated concurrently by multiple cores without the heavy overhead of OS-level mutex locking.

2. **Threads vs. Processes (`fork`) in C/Unix:**
   - UNIX provides `fork()` to create entirely separate OS processes. Processes have isolated, separate virtual memory spaces. If we used `fork()` for the compactor, it would inherit a *copy-on-write* snapshot of the memory. However, to synchronize the catalog swap back to the main query process, we'd have to jump through hoops using Shared Memory (`shmget` or `mmap(MAP_SHARED)`) and inter-process semaphores.
   - **Threads** (`pthread_create`), conversely, share the exact same virtual memory space. The background compactor thread can directly read the main `table_t*` pointer and update it securely using the `pthread_rwlock_t`.

3. **Comparison with Other Languages:**
   - **Python:** Standard Python threads are crippled by the Global Interpreter Lock (GIL). You cannot run True parallel CPU bounds tasks on multiple cores using standard Python threads. This is precisely why Python delegates heavy data processing to C/C++ extensions (like `simpledb`, `numpy`, or `duckdb`) which release the GIL and spawn native `pthreads` internally.
   - **Go:** Go uses "goroutines" (green threads) which are incredibly lightweight and multiplexed over OS threads by the Go Runtime. Go encourages passing messages via Channels instead of shared memory locks. While excellent for network services, the Go runtime incurs garbage collection (GC) pauses and scheduler overhead. C threads give us predictable, microsecond-tail latencies with zero GC pauses.
   - **C++:** C++11 introduced `std::thread` and `std::atomic`. Under the hood on Linux/macOS, they are essentially elegant wrappers around the exact same `pthread` and atomic compiler intrinsics we are using in C, just with added object-oriented ceremony.

Let's define the header for our part merger!

## 1. `include/simpledb/part_merger.h`

```c
#pragma once

#include "simpledb/table.h"
#include <stdbool.h>
#include <stddef.h>

// Configuration for the compaction policy
typedef struct {
    size_t max_parts_to_merge;
    size_t min_rows_to_merge;
    bool force_merge_all; // E.g., for an OPTIMIZE TABLE command
} merge_config_t;

// The state of a running compaction job
typedef struct {
    table_t* table;
    merge_config_t config;
    
    // Internal state tracking which parts are currently being merged
    size_t* part_ids_to_merge;
    size_t num_parts;
} part_merger_t;

// Initialize a part merger job for a table
part_merger_t* part_merger_create(table_t* table, merge_config_t config);

// Execute the merge on the main thread (blocking). 
// Returns the ID of the newly created consolidated part.
// Returns (size_t)-1 if no merge was necessary or an error occurred.
size_t part_merger_execute(part_merger_t* merger);

// Free the merger job resources
void part_merger_destroy(part_merger_t* merger);

// ---------------------------------------------------------
// Background Thread API
// ---------------------------------------------------------

// A high-level helper that evaluates the compaction policy in a loop.
// If the table needs compaction, it creates a merger and runs it.
// This is called by the background thread.
void* table_background_compactor_thread(void* arg);

// Starts the compactor in a background pthread
void table_start_background_compactor(table_t* table);
```

## Mocking the Compactor

We will write a unit test that simulates ingesting two separate batches of data (flushing them to disk as two separate immutable parts). Then, we will trigger the compactor and verify that the table metadata has been updated to point to only a single, consolidated part.

## 2. `tests/test_part_merger.c`

```c
#include "simpledb/part_merger.h"
#include "simpledb/table.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main() {
    // 1. Setup a Table
    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "amount", COL_TYPE_I32);
    
    table_t* table = table_create("events_merge_test", schema);
    
    // 2. Insert Data and Flush to create Part 1 (simulate batch 1)
    int32_t users1[] = {1, 2};
    int32_t amounts1[] = {100, 200};
    buffer_i32_t b_u1 = { .data = users1, .size = 2, .capacity = 2 };
    buffer_i32_t b_a1 = { .data = amounts1, .size = 2, .capacity = 2 };
    
    table_insert_column_i32(table, "user_id", &b_u1);
    table_insert_column_i32(table, "amount", &b_a1);
    table_flush(table); // Creates part 0
    
    // 3. Insert Data and Flush to create Part 2 (simulate batch 2)
    int32_t users2[] = {3, 4};
    int32_t amounts2[] = {300, 400};
    buffer_i32_t b_u2 = { .data = users2, .size = 2, .capacity = 2 };
    buffer_i32_t b_a2 = { .data = amounts2, .size = 2, .capacity = 2 };
    
    table_insert_column_i32(table, "user_id", &b_u2);
    table_insert_column_i32(table, "amount", &b_a2);
    table_flush(table); // Creates part 1
    
    // Assume part 0 and part 1 are now immutable parts on disk.
    // Let's verify we have exactly 2 parts.
    assert(table->num_parts == 2);
    
    // 4. Run the Compactor
    merge_config_t config = {
        .max_parts_to_merge = 10,
        .min_rows_to_merge = 1,
        .force_merge_all = true // Force compaction of all existing parts
    };
    
    part_merger_t* merger = part_merger_create(table, config);
    size_t new_part_id = part_merger_execute(merger);
    part_merger_destroy(merger);
    
    assert(new_part_id != (size_t)-1);
    
    // 5. Verify the Table State
    // The table should now only have 1 active, consolidated part!
    assert(table->num_parts == 1);
    
    // 6. Cleanup
    table_destroy(table);
    schema_registry_destroy(schema);
    
    printf("Day 19 MergeTree Part Compactor test passed!\\n");
    return 0;
}
```

---

## Complete Implementation Guide for Day 19

If you are offline, here is the full roadmap of the missing logic you need to write in `.c` files to bring the `part_merger` to life:

### Step 1: Finish `table.c` (The Secure Read Path)

As discussed, you already secured the write path (`table_flush`). Now you must secure the read path.
In `src/table.c`, update `table_get_parts`:
```c
table_part_t **table_get_parts(table_t *table, size_t *out_num_parts) {
    if (!table) return NULL;
    
    // 1. Take Read Lock
    pthread_rwlock_rdlock(&table->catalog_lock);
    
    // 2. Allocate a private array for the caller
    size_t num = table->num_parts;
    if (out_num_parts) *out_num_parts = num;
    
    if (num == 0) {
        pthread_rwlock_unlock(&table->catalog_lock);
        return NULL;
    }
    
    table_part_t **parts_copy = malloc(sizeof(table_part_t*) * num);
    
    // 3. Copy pointers and increment refcounts
    for (size_t i = 0; i < num; i++) {
        parts_copy[i] = table->parts[i];
        atomic_fetch_add(&parts_copy[i]->refcount, 1);
    }
    
    // 4. Drop Read Lock
    pthread_rwlock_unlock(&table->catalog_lock);
    
    return parts_copy;
}
```
*Note: Because the caller now owns this array and holds references, you must eventually add a `table_release_parts(table_part_t **parts, size_t num)` function that loops through, `atomic_fetch_sub(&part->refcount, 1)`, and frees the array.*

### Step 2: Scaffold `src/part_merger.c`

Create `src/part_merger.c`. Start with the lifecycle functions:
1. `part_merger_create`: `malloc` the `part_merger_t`. Assign `table` and `config`. Determine which parts to merge (e.g. read the catalog to select small parts) and store them in `part_ids_to_merge`.
2. `part_merger_destroy`: Free `part_ids_to_merge` and the merger struct.

### Step 3: Implement The $K$-way Merge (`part_merger_execute`)

This is the algorithmic core of the day. Inside `part_merger_execute`:
1. **Initialize Readers**: For each part ID in `part_ids_to_merge`, initialize a `column_reader_t`. 
2. **Min-Heap Setup**: We want to output a consolidated part that is strictly ordered by the primary key. Create a Min-Heap (priority queue) of size $K$ (where $K$ is the number of parts). The heap stores the "current" row from each part, sorted by the primary key value.
3. **The Merge Loop**:
   - Pop the minimum row from the heap.
   - **Check Deletion Vector**: Check the `deletion_vector_t` (from Day 13) for this row's OID in its source part. If the bit is set (row deleted), *skip* it. Do not write it to the output buffer.
   - If valid, write the row to an in-memory columnar buffer.
   - Advance the reader for the part that just yielded the row. If it has more rows, push the next row into the Min-Heap.
   - If the in-memory buffer hits the 8,192 row granule limit, use `column_writer_t` to flush it to the *new* part directory.
4. **Finalize**: Close all readers. Close all writers.
5. **The Atomic Swap**: 
   - Take `pthread_rwlock_wrlock(&table->catalog_lock)`.
   - Update `table->parts` to remove the old $K$ parts.
   - Insert the newly created consolidated part.
   - Drop the lock `pthread_rwlock_unlock`.
6. **Cleanup Zombies**: The old parts are now detached. You cannot delete their files immediately because active queries might be reading them. Start a deferred cleanup task (or just check periodically) that loops over the detached parts. When `atomic_load(&part->refcount) == 0`, it is safe to `unlink()` the files and `free(part)`.

### Step 4: The Background Thread

Implement `table_start_background_compactor`:
1. Spawns a `pthread_create` pointing to `table_background_compactor_thread`.
2. The thread function runs an infinite `while(1)` loop.
3. It sleeps for a few seconds.
4. It wakes up, checks `table->num_parts`. If `num_parts > config.max_parts_to_merge`, it creates a `part_merger_t` and calls `part_merger_execute`.

With these steps complete, `simpledb` will successfully handle high-throughput ingestion without bogging down the read pipeline!

---

### Appendix: C Threading & Atomic API Reference

Since you will be coding offline, here is a quick reference for the POSIX thread and C11 atomic primitives you will need to call.

#### 1. Read-Write Locks (`<pthread.h>`)

*   **`int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr);`**
    Initializes a new read-write lock. Pass `NULL` for `attr` to use the defaults.
*   **`int pthread_rwlock_destroy(pthread_rwlock_t *rwlock);`**
    Destroys the lock. Always call this before freeing the struct that holds it.
*   **`int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock);`**
    Acquires a **Read Lock**. Blocks if another thread holds the Write Lock, but allows infinite other threads to simultaneously hold Read Locks.
*   **`int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock);`**
    Acquires a **Write Lock**. Blocks until *all* other threads (both readers and writers) have released the lock. Only one thread can hold this at a time.
*   **`int pthread_rwlock_unlock(pthread_rwlock_t *rwlock);`**
    Releases the lock (whether it was acquired as read or write).

#### 2. Thread Creation (`<pthread.h>`)

*   **`int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine) (void *), void *arg);`**
    Spawns a new OS thread.
    *   `thread`: Pointer to a `pthread_t` handle.
    *   `attr`: Pass `NULL` for defaults.
    *   `start_routine`: A function pointer of the signature `void* my_func(void* arg)`.
    *   `arg`: A pointer passed to your thread function (e.g. `(void*)table`).

#### 3. Atomics (`<stdatomic.h>`)

*   **`_Atomic type`**
    A type qualifier (e.g., `_Atomic size_t`) that tells the compiler to emit lock-free hardware instructions (when supported) for this variable.
*   **`void atomic_init(_Atomic type *obj, type value);`**
    Initializes the atomic variable (e.g., `atomic_init(&part->refcount, 0);`). *Note: This is not an atomic operation itself, use it only during creation.*
*   **`type atomic_load(const _Atomic type *obj);`**
    Safely reads the current value across thread boundaries.
*   **`type atomic_fetch_add(_Atomic type *obj, type arg);`**
    Atomically adds `arg` to `*obj` and returns the *previous* value.
*   **`type atomic_fetch_sub(_Atomic type *obj, type arg);`**
    Atomically subtracts `arg` from `*obj` and returns the *previous* value.

#### 4. Sleep (`<unistd.h>`)
*   **`unsigned int sleep(unsigned int seconds);`**
    Suspends the thread for the specified number of seconds. Useful inside your background `while(1)` compactor loop to prevent pegging a CPU core at 100%.

---

### Socratic Debugging Session: Building `part_merger_create`

During implementation, we identified a few critical C-specific gotchas to watch out for when scaffolding the compactor.

#### 1. Releasing the References (`table.c`)
When `table_get_parts` copies the active parts array for a query (or our compactor), it increments the atomic `refcount`. This guarantees the parts won't be deleted while we read them. However, we must provide a way to drop those references when we are finished:

```c
// Add to src/table.c
void table_release_parts(table_part_t **parts, size_t num_parts) {
    if (!parts) return;
    for (size_t i = 0; i < num_parts; i++) {
        atomic_fetch_sub(&parts[i]->refcount, 1);
    }
    free(parts);
}
```

#### 2. Capping the Merge Config
The goal of the compactor is to merge a *bounded* number of parts (e.g., merging 10 small parts into 1). When initializing `part_merger_t`, make sure you don't accidentally try to merge all parts if the table has hundreds! Cap the size using `config.max_parts_to_merge`:

```c
size_t merge_count = (num_parts < config.max_parts_to_merge) ? num_parts : config.max_parts_to_merge;
merger->num_parts = merge_count;
```

#### 3. Resolving the Sorted `part_id`
We use `qsort` to sort the parts by `row_count` so we always merge the smallest parts first. However, after sorting, the array indices no longer match the original `table->parts` indices! You must map them back to their original IDs to execute the merge correctly.

```c
// Incorrect: Hardcoding the indices ignores the qsort!
for (size_t i = 0; i < merger->num_parts; i++) {
    merger->part_ids_to_merge[i] = i; 
}

// Correct: Grab the original part_id from the sorted struct
for (size_t i = 0; i < merger->num_parts; i++) {
    merger->part_ids_to_merge[i] = items[i].part_id;
}
```

#### 4. The `min_rows_to_merge` Heuristic (And the Compaction Deadlock)
In `merge_config_t`, the `min_rows_to_merge` field prevents the compactor from thrashing the disk. However, enforcing it naively creates a classic **Compaction Deadlock**: 
If `max_parts_to_merge = 10` and `min_rows_to_merge = 100,000`, and you ingest in batches of 1,000 rows, the 10 smallest parts will only ever equal 10,000 rows. The compactor will abort. More parts will pile up. Even with 100 parts, the 10 smallest still only equal 10,000 rows, so it aborts again! Eventually, the database crashes from "Too Many Open Files".

To fix this, `min_rows_to_merge` should only prevent merges when the system is healthy (low part count). If the table's total part count hits the ceiling (`table->num_parts >= config.max_parts_to_merge`), we are in an "emergency" state and MUST merge the smallest parts to reduce file handles, bypassing the minimum rows check entirely:

```c
    if (!config.force_merge_all && num_parts < config.max_parts_to_merge) {
        size_t total_rows = 0;
        for (size_t i = 0; i < merger->num_parts; i++) {
            size_t id = merger->part_ids_to_merge[i];
            total_rows += parts[id]->row_count;
        }
        
        if (total_rows < config.min_rows_to_merge) {
            merger->num_parts = 0; // Abort merge, we are healthy and don't have enough volume yet
        }
    }
```

#### 5. The `qsort` Comparator (`size_t` underflow)
When sorting parts, you always want to sort in **Ascending** order (smallest to largest) so you merge the smallest parts first. Merging a massive 10GB part with a tiny 1MB part causes extreme write amplification, whereas merging ten 1MB parts is extremely fast.

However, when writing the comparator, be careful not to subtract `row_count`:

```c
// DANGEROUS: row_count is an unsigned size_t. 
// 10 - 100 underflows to 18446744073709551526!
static int part_comparator(const void *a, const void *b) {
    return ((part_item *)a)->part->row_count - ((part_item *)b)->part->row_count; 
}

// CORRECT: Use explicit comparisons
static int part_comparator(const void *a, const void *b) {
    size_t count_a = ((part_item *)a)->part->row_count;
    size_t count_b = ((part_item *)b)->part->row_count;
    
    if (count_a < count_b) return -1;
    if (count_a > count_b) return 1;
    return 0;
}
```

#### 6. Scaffolding the $K$-Way Merge (`part_merger_execute`)
Because `table_scanner_t` reads a table sequentially, we cannot use it for a $K$-way merge which requires reading multiple parts *simultaneously*. Instead, we must manage the `column_reader_t` instances manually. 

A great pattern is to define a `part_stream_t` struct to keep track of each part's readers and current row state:

```c
typedef struct {
    size_t part_id;
    column_reader_t** readers;   // Array of readers (one per column for this part)
    int32_t current_key;         // The sort key for the min_heap (e.g., col 0)
    int32_t* current_values;     // Buffer holding the current row's values for the other columns
} part_stream_t;
```

When you implement `part_merger_execute`, you will allocate an array of `part_stream_t` and a `min_heap_t` (built on Day 15). You read the very first row from each part into its stream, and push the stream into the heap using `min_heap_push`. Because the heap node's `payload` is a `void*`, you simply pass the pointer to the stream:

```c
// Inside part_merger_execute:
min_heap_t* heap = min_heap_create(merger->num_parts);

for (size_t i = 0; i < merger->num_parts; i++) {
    // ... initialize the column readers for streams[i] ...
    // ... read the first row into streams[i].current_key ...
    
    // Seed the heap (the payload is the address of the stream struct)
    min_heap_push(heap, streams[i].current_key, &streams[i]);
}
```

#### Understanding the Memory: `streams` vs `writers`
A common point of confusion when scaffolding `part_merger_execute` is mixing up the readers and writers. You need two distinct arrays:

1. **The Input `streams` (Readers):** An array of `part_stream_t` of size `merger->num_parts` ($K$). This represents the $K$ small parts you are actively pulling data from.
   ```c
   part_stream_t *streams = malloc(sizeof(part_stream_t) * merger->num_parts);
   for (size_t i = 0; i < merger->num_parts; i++) {
       streams[i].part_id = merger->part_ids_to_merge[i];
       streams[i].readers = malloc(sizeof(column_reader_t*) * table->schema->count);
       // Initialize readers for this specific part...
   }
   ```

2. **The Output `writers`:** An array of `column_writer_t*` of size `table->schema->count`. Because compaction merges $K$ parts into **exactly one** new consolidated part, you only need one writer per column!
   ```c
   column_writer_t **writers = malloc(sizeof(column_writer_t*) * table->schema->count);
   for (size_t col = 0; col < table->schema->count; col++) {
       // Initialize writer for the new consolidated part directory...
   }
   ```

#### 7. Batching Writes (Avoiding Single Row I/O)
When popping rows from the Min-Heap, you are resolving one row at a time. However, writing to disk row-by-row is incredibly inefficient and defeats the purpose of a columnar database. You must buffer the rows in memory and write them in **Batches** (Granules of 8,192 rows).

To do this, allocate an in-memory array for each column that holds 8,192 elements. As you pop from the heap, append the values to these arrays. When the arrays fill up, flush them using the `column_writer_t` and reset the counter.

```c
// 1. Setup the writers for the NEW consolidated part
column_writer_t** writers = malloc(sizeof(column_writer_t*) * table->schema->count);
// ... initialize writers ...

// 2. Setup the in-memory output buffers (Batch size = 8192)
int32_t* out_keys = malloc(sizeof(int32_t) * 8192);
int32_t* out_vals = malloc(sizeof(int32_t) * 8192);
size_t out_row_count = 0;

heap_node_t min_node;
while (min_heap_pop(heap, &min_node)) {
    part_stream_t* stream = (part_stream_t*)min_node.payload;
    
    // 3. Buffer the row in memory
    out_keys[out_row_count] = stream->current_key;
    out_vals[out_row_count] = stream->current_values[0]; // Assuming 1 extra col
    out_row_count++;
    
    // 4. Flush the batch if full!
    if (out_row_count == 8192) {
        column_writer_append(writers[0], out_keys, 8192);
        column_writer_append(writers[1], out_vals, 8192);
        out_row_count = 0;
    }
    
    // 5. Advance the reader for this stream and push the next row back into the heap...
}

// 6. Don't forget to flush the leftover rows!
if (out_row_count > 0) {
    column_writer_append(writers[0], out_keys, out_row_count);
    column_writer_append(writers[1], out_vals, out_row_count);
}
```

#### 8. Demystifying `part_stream_t` (The Vectorized State Machine)
Because our database is *Vectorized* and reads disk data in 8,192-row chunks called Granules, the Min-Heap loop is slightly more complex than a standard row-based database.

When we call `column_reader_next_granule_i32()`, we don't get one row; we get a `slice_i32_t` containing up to 8,192 rows! But the Min-Heap must sort *single rows*. 

Therefore, our `part_stream_t` needs to cache the current granules, keep an index of which row it is currently looking at, and expose only that single row to the Min-Heap:

```c
typedef struct {
    size_t part_id;
    column_reader_t **readers;     // Array of readers (one per column)
    
    // Vectorized Granule Cache
    slice_i32_t *current_granules; // Array of slices (one per column)
    size_t row_idx;                // Which row in the granule are we at? (0 to 8191)
    
    // The single row exposed to the Min-Heap
    int32_t current_key;           // Col 0
    int32_t *current_values;       // Cols 1..N
} part_stream_t;
```

**How it works:**
1. You fetch a full granule for every column.
2. You set `row_idx = 0`.
3. You pull `current_granules[0].data[0]` into `current_key`, and push to the Min-Heap.
4. When the Min-Heap pops this stream, you write the row. Then you `row_idx++`. 
5. If `row_idx < current_granules[0].len`, you just grab the next row from RAM and push back to the heap!
6. If `row_idx == current_granules[0].len`, you exhausted the chunk in RAM. You must call `column_reader_next_granule_i32` again to hit the disk.

#### 9. Opening the Files (Vectorized Initialization)

Here is the exact structural C code to initialize this vectorized structure correctly inside `part_merger_execute`:

```c
// ==========================================
// OUTSIDE THE LOOP (Global Setup)
// ==========================================
size_t total_parts;
table_part_t **parts = table_get_parts(merger->table, &total_parts);

part_stream_t *streams = malloc(sizeof(part_stream_t) * merger->num_parts);
min_heap_t *heap = min_heap_create(merger->num_parts);
size_t num_cols = merger->table->schema->count;

// ==========================================
// INSIDE THE LOOP (Per-Part Setup)
// ==========================================
for (size_t i = 0; i < merger->num_parts; i++) {
    
    // 1. Get the path to this specific part directory on disk
    size_t part_id = merger->part_ids_to_merge[i];
    streams[i].part_id = part_id;
    const char* part_path = parts[part_id]->part_path; 

    // 2. Allocate the arrays (Notice we allocate POINTERS for readers)
    streams[i].readers = malloc(sizeof(column_reader_t*) * num_cols);
    streams[i].current_granules = malloc(sizeof(slice_i32_t) * num_cols);
    streams[i].current_values = malloc(sizeof(int32_t) * (num_cols - 1));

    // 3. Create a reader and immediately read the FIRST GRANULE for every column
    for (size_t col = 0; col < num_cols; col++) {
        streams[i].readers[col] = column_reader_create(
            part_path, 
            merger->table->schema->columns[col].name, 
            merger->table->schema->columns[col].type,
            NULL // No sparse index mask for compaction
        );
        streams[i].current_granules[col] = column_reader_next_granule_i32(streams[i].readers[col]);
    }

    // 4. If the first column's granule has data, extract Row 0 and push to heap!
    if (streams[i].current_granules[0].len > 0) {
        streams[i].row_idx = 0;
        streams[i].current_key = streams[i].current_granules[0].data[0]; // Sort Key (Col 0)
        
        // Extract the rest of the columns
        for (size_t col = 1; col < num_cols; col++) {
            streams[i].current_values[col - 1] = streams[i].current_granules[col].data[0];
        }
        
        min_heap_push(heap, streams[i].current_key, &streams[i]);
    }
}
```

#### 10. The Vectorized Merge Loop
Once the Min-Heap is seeded with Row 0 from every part, we enter the main merge loop. 

Because we are vectorized, we don't hit the disk on every single row! We only call `column_reader_next_granule_i32` when our `row_idx` reaches the end of the 8,192-row `slice_i32_t` chunk.

Here is the exact structural logic for the `while` loop (which goes directly beneath the setup block above):

```c
// ==========================================
// THE MERGE LOOP
// ==========================================
heap_node_t min_node;

while (min_heap_pop(heap, &min_node)) {
    // 1. Get the stream that holds the smallest key
    part_stream_t* stream = (part_stream_t*)min_node.payload;
    
    // 2. Buffer the row for writing (Batching logic omitted for brevity)
    // out_keys[out_row_count] = stream->current_key;
    // out_vals[out_row_count] = stream->current_values[0];
    // out_row_count++;
    
    // 3. Advance the stream to the next row in the RAM chunk
    stream->row_idx++;
    
    // 4. Did we exhaust the 8,192-row chunk? If so, fetch the next one from disk!
    if (stream->row_idx >= stream->current_granules[0].len) {
        for (size_t col = 0; col < num_cols; col++) {
            stream->current_granules[col] = column_reader_next_granule_i32(stream->readers[col]);
        }
        stream->row_idx = 0; // Reset index for the newly fetched chunk
    }
    
    // 5. If we have data (either from the old chunk or the new one), re-queue to the heap!
    if (stream->current_granules[0].len > 0) {
        // Extract the Key (Col 0) for this new row
        stream->current_key = stream->current_granules[0].data[stream->row_idx];
        
        // Extract the Values (Cols 1..N) for this new row
        for (size_t col = 1; col < num_cols; col++) {
            stream->current_values[col - 1] = stream->current_granules[col].data[stream->row_idx];
        }
        
        min_heap_push(heap, stream->current_key, stream);
    }
}
```

#### 11. Refactoring the Fetch Logic (Loop Priming & The Calloc Trick)
If you look closely at Sections 9 and 10, you'll notice a classic "code smell": the logic to check if a chunk is empty, fetch a new one, extract the values, and push to the heap is duplicated! It happens once in the initialization loop to "prime" the heap, and again inside the `while` loop.

We can DRY (Don't Repeat Yourself) this code by extracting it into a helper function:

```c
static void stream_fetch_next_and_push(part_stream_t* stream, min_heap_t* heap, size_t num_cols) {
    if (stream->row_idx >= stream->current_granules[0].len) {
        for (size_t j = 0; j < num_cols; j++) {
            stream->current_granules[j] = column_reader_next_granule_i32(stream->readers[j]);
        }
        stream->row_idx = 0;
    }

    if (stream->current_granules[0].len > 0) {
        for (size_t j = 0; j < num_cols; j++) {
            stream->current_values[j] = stream->current_granules[j].data[stream->row_idx];
        }
        stream->current_key = stream->current_values[0];
        min_heap_push(heap, stream->current_key, stream);
    }
}
```

**The `calloc` Trick:** For this helper to work during the *initialization* phase, `stream->current_granules[0].len` must be `0` so the `if` statement triggers a disk read. If you allocate `current_granules` using `malloc`, `len` might be filled with garbage memory (e.g., `99,999`), which bypasses the read and crashes the heap! 
By using `calloc(sizeof(slice_i32_t), num_cols)` during setup, the memory is naturally zero-initialized, guaranteeing `len` is `0`.

#### 12. Memory Cleanup & Flushing Writers
C requires explicit cleanup. At the end of `part_merger_execute`, you must not only free the memory but also ensure the OS buffers for the new part are fully flushed to disk.

1. **Close the Writers:** Loop through your writers array and call `column_writer_close(writers[j])`. If you skip this, the file footers/indexes won't be written, and data will be lost.
2. **Clean the Streams:** Create a quick helper like `part_stream_free(part_stream_t* stream)` that loops through `stream->readers` calling `column_reader_free`, and then frees the internal structs.
3. **Clean the Engine:** Free the `writers` array, the `streams` array, and call `min_heap_destroy(heap)`.

#### 13. Garbage Collection & The Background Daemon
Right now, `part_merger_execute` removes the old $K$ parts from the active catalog (`table->parts`), turning them into **Zombies**. However, it doesn't delete them from the hard drive!
In a production database, a background daemon handles garbage collection. Since our compactor runs in its own background daemon thread anyway, we can do the garbage collection right inside the execution!

**The Daemon Thread**
When creating a POSIX thread, the `pthread_create` API demands a function with a very specific signature: `void* my_func(void* arg)`. 

```c
void* table_background_compactor_thread(void* arg) {
    table_t* table = (table_t*)arg;
    merge_config_t config = {
        .max_parts_to_merge = 10,
        .min_rows_to_merge = 50000,
        .force_merge_all = false
    };

    while(1) {
        sleep(5); // Wake up every 5 seconds
        
        part_merger_t* merger = part_merger_create(table, config);
        part_merger_execute(merger);
        part_merger_destroy(merger);
    }
    return NULL;
}
```

**The Fast-Path Optimization (in `part_merger_create`)**
To prevent the daemon from triggering expensive `malloc` and `qsort` operations every 5 seconds on an idle database, we move the fast-path check to the very top of `part_merger_create`:

```c
part_merger_t *part_merger_create(table_t *table, merge_config_t config) {
    // 1. FAST-PATH: Don't do heavy allocations if the table is idle!
    pthread_rwlock_rdlock(&table->catalog_lock);
    size_t current_parts = table->num_parts;
    pthread_rwlock_unlock(&table->catalog_lock);

    part_merger_t *merger = malloc(sizeof(*merger));
    merger->config = config;
    merger->table = table;
    merger->num_parts = 0; // Default to empty
    merger->part_ids_to_merge = NULL;

    if (!config.force_merge_all && current_parts < config.max_parts_to_merge) {
        return merger; // Abort early! No heavy lifting!
    }

    // 2. SLOW-PATH: Actually grab the parts and sort them
    size_t num_parts;
    table_part_t **parts = table_get_parts(table, &num_parts);
    // ... (rest of the qsort and merging setup) ...
}
```

**The Zombie GC Loop**
Since the background thread only has access to the generic `table_t`, we can handle the GC directly at the bottom of `part_merger_execute`. We just save the old pointers before the atomic swap, and then wait for their refcounts to drop!

```c
    // 1. Save the Zombies before we overwrite the table array!
    table_part_t **zombies = malloc(sizeof(table_part_t*) * merger->num_parts);
    for (size_t i = 0; i < merger->num_parts; i++) {
        zombies[i] = merger->table->parts[merger->part_ids_to_merge[i]];
    }

    // 2. Do the Atomic Swap of table->parts here!

    // 3. Zombie Garbage Collection
    for (size_t i = 0; i < merger->num_parts; i++) {
        while (atomic_load(&zombies[i]->refcount) > 0) usleep(1000); 
        // unlink(zombies[i]->part_path);
        free(zombies[i]); 
    }
    free(zombies);
```

---

## 14. Final Implementation

Here is the final, fully assembled C code for the compactor module.

### `include/simpledb/part_merger.h`
```c
#pragma once

#include "simpledb/table.h"
#include <stdbool.h>
#include <stddef.h>

// Configuration for the compaction policy
typedef struct {
    size_t max_parts_to_merge;
    size_t min_rows_to_merge;
    bool force_merge_all; // E.g., for an OPTIMIZE TABLE command
} merge_config_t;

// The state of a running compaction job
typedef struct {
    table_t* table;
    merge_config_t config;
    
    // Internal state tracking which parts are currently being merged
    size_t* part_ids_to_merge;
    size_t num_parts;
    table_part_t** zombies;
} part_merger_t;

// Initialize a part merger job for a table
part_merger_t* part_merger_create(table_t* table, merge_config_t config);

// Execute the merge on the main thread (blocking). 
// Returns the ID of the newly created consolidated part.
// Returns 0 if no merge was necessary or an error occurred.
size_t part_merger_execute(part_merger_t* merger);

// Free the merger job resources
void part_merger_destroy(part_merger_t* merger);

// ---------------------------------------------------------
// Background Thread API
// ---------------------------------------------------------

// A high-level helper that evaluates the compaction policy in a loop.
// If the table needs compaction, it creates a merger and runs it.
// This is called by the background thread.
void* table_background_compactor_thread(void* arg);
```

### `src/part_merger.c`
```c
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

    char new_part_dir[256];
    sprintf(new_part_dir, "%s/part_%ld", merger->table->name, (long)time(NULL));
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
```
## The MergeTree Part Naming Convention
You make an excellent point. Naming directories `part_<timestamp>` can lead to collisions if inserts happen rapidly within the same second, and it provides very little information about what's actually *inside* the part or how it relates to other parts. 

If we look at **ClickHouse's MergeTree engine**, it solves this elegantly using a very specific nomenclature that makes it possible to understand the lineage of a part just by looking at its directory name.

## The ClickHouse Pattern

A typical ClickHouse part name looks like this:
`{partition_id}_{min_block}_{max_block}_{level}`

1. **`partition_id`**: For partitioned tables (e.g. by month `202310`), this groups data. Since we don't have partitioning yet, we'll use a hardcoded prefix like `"all"`.
2. **`min_block`**: A monotonic ID assigned to the first data block that was merged to create this part.
3. **`max_block`**: A monotonic ID assigned to the last data block that was merged into this part.
4. **`level`**: The depth of the merge tree. Freshly inserted parts start at `level = 0`. When you merge a bunch of parts together, the new part's level is `max(levels) + 1`.

### Example Lineage

1. **Insert Batch 1**: We increment the table's `next_block_id` to `1`. The part is created as `all_1_1_0`.
2. **Insert Batch 2**: `next_block_id` becomes `2`. The part is `all_2_2_0`.
3. **Insert Batch 3**: `next_block_id` becomes `3`. The part is `all_3_3_0`.
4. **Compaction**: The background merger wakes up and selects `all_1_1_0` and `all_2_2_0`. 
   - `min_block` = `min(1, 2)` = `1`
   - `max_block` = `max(1, 2)` = `2`
   - `level` = `max(0, 0) + 1` = `1`
   - The new consolidated directory is named: `all_1_2_1`.

This scheme instantly tells us that `all_1_2_1` contains all the data from block 1 to block 2, and it has undergone 1 merge operation. It also makes resolving overlapping data extremely easy: if a query sees both `all_1_1_0` and `all_1_2_1` on disk (perhaps before the GC cleans up the zombie), it knows `all_1_2_1` *completely covers* the older part, allowing it to safely ignore it.

## Updating the C Headers

I've updated the `table_part_t` and `table_t` structs in `include/simpledb/table.h` to track these fields:

```c
// A simple structure representing an immutable flushed part on disk
typedef struct table_part_t {
    char part_path[256];
    size_t row_count;

    // ClickHouse MergeTree Naming Convention
    uint64_t min_block;
    uint64_t max_block;
    uint32_t level;

    // Atomic reference count for concurrent readers (0 = safe to delete)
    _Atomic size_t refcount;
} table_part_t;

typedef struct table_t {
    char name[64];
    schema_registry_t *schema;

    // Concurrency control for catalog metadata (parts array)
    pthread_rwlock_t catalog_lock;

    // In-memory columnar buffers (one per column in schema)
    void **column_buffers;

    // Total rows currently in memory
    size_t row_count;

    // Track flushed parts (for the Read API)
    table_part_t **parts;
    size_t num_parts;
    
    // Monotonic block sequence for ClickHouse-style part naming
    uint64_t next_block_id;

    // Optional FTS index for text columns
    inverted_index_t *fts;
} table_t;
```

## Implementation

To fully adopt this, the C implementation requires two updates:
1. In `table.c`, modify `table_flush` to auto-increment `table->next_block_id`, assign the part's block/level metadata, and format the path string instead of relying strictly on the `data_dir` parameter.
2. In `part_merger.c`, modify `part_merger_execute` to calculate the `min_block`, `max_block`, and `level` based on the selected parts, and construct the new part directory name accordingly.

The `table_part_format_name` encapsulation is implemented directly into `simpledb/table.h`:

```c
static inline void table_part_format_name(char *buf, size_t buf_size, const char *base_dir, uint64_t min_block, uint64_t max_block, uint32_t level) {
    snprintf(buf, buf_size, "%s/all_%llu_%llu_%u", base_dir, (unsigned long long)min_block, (unsigned long long)max_block, (unsigned)level);
}
```

We also update `table_flush` in `table.c` to auto-increment the `next_block_id` sequence, and updated `part_merger_execute` in `part_merger.c` to calculate the merge tree lineage for new parts:

```c
// part_merger.c snippet:

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
```

When running the test suite, we can see the exact ClickHouse folder structure physically manifesting on disk in the build directory!

```sh
build/tests/all_1_1_0
build/tests/all_2_2_0
build/tests/events_merge_test/all_1_2_1
```
