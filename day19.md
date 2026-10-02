# Day 19: The MergeTree Part Compactor (Background Part Merging Engine)

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
