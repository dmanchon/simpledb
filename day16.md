# Day 16: Partitioned Grace Hash Aggregation (Disk Spilling GROUP BY)

### Architectural Overview: The `GROUP BY` Challenge

In SQL analytics, `GROUP BY` is fundamental for answering "how many" or "how much" per category. For small cardinalities (e.g., grouping by 50 US states), the database can easily maintain a hash table in CPU caches (L2/L3) and perform ultra-fast in-memory aggregations.

However, analytical databases must also handle **high-cardinality aggregations**—grouping by millions of unique users, IP addresses, or tracking IDs. When the number of distinct groups grows beyond what physical RAM can hold, an in-memory hash table will encounter frequent cache misses, and eventually trigger an Out-Of-Memory (OOM) panic or swap thrashing.

To aggregate arbitrarily large datasets under a strict memory budget, we use **Partitioned Grace Hash Aggregation**.

---

### The Grace Hash Aggregation Pipeline

Grace Hash Aggregation solves the unbounded memory problem by dividing it into two phases: **Partitioning** and **Aggregation**. 

#### Phase 1: Partitioning (Spilling to Disk)
As records stream in, we hash the `GROUP BY` key. We use this hash to assign the record to one of $P$ partitions (e.g., `hash(key) % P`).
Instead of attempting to keep all unique groups in memory simultaneously, we maintain small memory buffers for each partition. When a partition's buffer fills up, it is spilled to disk as a temporary run. 
This ensures that **records with the same group key always land in the same partition**, regardless of when they arrived!

#### Phase 2: In-Memory Aggregation (Merging)
Once all input has been partitioned, the aggregator processes each partition one by one.
Because we know that all rows for a given group key are contained within a single partition, we can safely load the entire partition into an in-memory hash table, perform the final aggregation (`SUM`, `COUNT`), and yield the results. After one partition is aggregated and its results emitted, its memory is cleared and reused for the next partition.

```text
Incoming Streaming Rows (Keys: 5, 2, 9, 5, 1, ...)
         │
         ▼ Hash(Key) % P
   ┌─────┴─────┬───────────┐
   ▼           ▼           ▼
[Part 0]    [Part 1]    [Part 2]
(Keys: 2)   (Keys: 9)   (Keys: 5, 1)
   │           │           │
 (Spill)     (Spill)     (Spill)
   │           │           │
   ▼           ▼           ▼
[Mem Hash]  [Mem Hash]  [Mem Hash]
   │           │           │
   ▼           ▼           ▼
 Results     Results     Results
```

By ensuring $P$ is large enough, each individual partition's unique groups will easily fit into the allocated memory limit, allowing `simpledb` to aggregate datasets infinitely larger than RAM without thrashing.

---

### C Header: The Grace Hash Aggregator API

We define the `grace_hash_agg_t` structure in C11 to manage the memory buffers, the file pointers for disk spills, and the state for the output phase.

#### `include/simpledb/grace_hash_agg.h`

```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// A record representing an input row for aggregation.
// We assume a simple GROUP BY (1 int32 key) and one SUM/COUNT aggregation (1 int32 value).
typedef struct {
    int32_t group_key;
    int32_t agg_value;
} agg_input_record_t;

// A record representing the aggregated output.
typedef struct {
    int32_t group_key;
    int64_t sum_value;
    int64_t count_value;
} agg_output_record_t;

// Grace Hash Aggregator state
typedef struct grace_hash_agg_t {
    char temp_dir[256];
    size_t num_partitions;
    size_t mem_limit_records;

    // In-memory buffer for incoming records before they are partitioned/spilled
    agg_input_record_t *in_mem_buf;
    size_t in_mem_count;

    // File pointers for each partition's spill file
    FILE **partition_files;
    char **partition_paths;
    
    // Total records spilled to each partition
    size_t *partition_counts;

    bool finished_ingestion;

    // For the output phase: current partition being aggregated in memory
    size_t current_partition_idx;
    
    // In-memory hash table for the current partition
    // (Mock implementation will use a simple array or similar structure)
    agg_output_record_t *current_hash_table;
    size_t hash_table_size;
    size_t hash_table_capacity;
    size_t hash_table_iter_idx;
    
} grace_hash_agg_t;

// Create and destroy the Grace Hash Aggregator
grace_hash_agg_t *grace_hash_agg_create(const char *temp_dir, size_t mem_limit_records, size_t num_partitions);
void grace_hash_agg_destroy(grace_hash_agg_t *agg);

// Push a batch of records into the aggregator. 
// If buffers fill up, records are hashed and spilled to partition files.
int grace_hash_agg_push(grace_hash_agg_t *agg, const agg_input_record_t *records, size_t count);

// Seal ingestion, flush all buffers to disk, and prepare for output phase
int grace_hash_agg_finish(grace_hash_agg_t *agg);

// Pull the next aggregated result. Returns true if a record was found, false on EOF.
bool grace_hash_agg_next(grace_hash_agg_t *agg, agg_output_record_t *out_record);
```

---

### Unit Testing Grace Hash Aggregation

We simulate a workload with 10 unique groups, pushing 500 records. With a memory limit of 100 records and 4 partitions, the aggregator will be forced to spill to disk during ingestion and load partitions back into memory during the `_next` yield phase.

#### `tests/test_db.c`

```c
static void test_grace_hash_agg(void) {
    printf("Testing Partitioned Grace Hash Aggregation...\n");
    // Memory limit of 100 records, 4 partitions
    grace_hash_agg_t *agg = grace_hash_agg_create("test_grace_agg_tmp", 100, 4);
    assert(agg != NULL);

    size_t total_records = 500; // Will force spilling
    agg_input_record_t *records = malloc(total_records * sizeof(agg_input_record_t));
    for (size_t i = 0; i < total_records; i++) {
        records[i].group_key = i % 10; // 10 unique groups
        records[i].agg_value = 5;
    }

    // Phase 1: Ingestion & Partitioning
    assert(grace_hash_agg_push(agg, records, total_records) == 0);
    assert(grace_hash_agg_finish(agg) == 0);

    // Phase 2: Aggregation & Merging
    agg_output_record_t out_rec;
    size_t out_count = 0;
    while (grace_hash_agg_next(agg, &out_rec)) {
        // We pushed 500 records. 10 groups, so each group has 50 records.
        // Sum should be 50 * 5 = 250, count should be 50.
        assert(out_rec.count_value == 50);
        assert(out_rec.sum_value == 250);
        out_count++;
    }

    assert(out_count == 10); // 10 unique groups

    grace_hash_agg_destroy(agg);
    free(records);
    printf("Grace Hash Aggregation tests passed!\n");
}
```

---

### Implementation Guide: Building Phase 1 (Ingestion & Spilling)

When translating the `grace_hash_agg_t` design into C, the boilerplate memory management is straightforward, but the spilling logic in `grace_hash_agg_push` requires careful orchestration.

**Step 1: The Setup (`_create` and `_destroy`)**
- Allocate the `grace_hash_agg_t` struct and `in_mem_buf` to hold `mem_limit_records * sizeof(agg_input_record_t)`.
- Allocate the arrays to hold the partition metadata (`partition_files`, `partition_paths`, `partition_counts`).
- In `_destroy`, ensure any open file pointers are closed, temporary partition files are deleted, and all arrays are freed.

**Step 2: Phase 1 Ingestion (`_push`)**
As rows stream in, you append them to `agg->in_mem_buf`. When `agg->in_mem_count == agg->mem_limit_records`, the buffer is full. At this point, you must execute the spill:
1. Loop over the in-memory buffer.
2. Compute the target partition using a simple modulo hash: `uint32_t p = record.group_key % agg->num_partitions;`.
3. Open that partition's file in append mode (`"ab"`).
4. `fwrite` the record and increment `partition_counts[p]`.
5. Once the loop finishes, reset `in_mem_count = 0` and resume accepting new rows.

**Step 3: Sealing the Ingestion (`_finish`)**
When the query engine has exhausted the input table, it calls `_finish()`.
- Perform one final spill: iterate over the *remaining* records currently residing in `in_mem_buf` (up to `in_mem_count`) and write them out to their respective partitions just like in `_push`.
- Set `agg->finished_ingestion = true`.

This completes Phase 1! The records are now safely resting on disk, strictly partitioned by their group keys.

---

### Socratic Q&A: Memory Management

**Q: Should the dynamic arrays for partitions and buffers be allocated inside `_create`?**
Yes! In C, `_create` acts as the constructor. Since `num_partitions` and `mem_limit_records` are only known at runtime, the struct's internal arrays (`in_mem_buf`, `partition_files`, `partition_paths`, `partition_counts`) must be dynamically allocated inside `_create` immediately after allocating the main struct. This ensures the struct is fully initialized and ready to use by the time it is returned to the caller. Conversely, `_destroy` acts as the destructor, where you must walk through and free all these dynamically allocated arrays before freeing the main struct itself.

**Q: What are some common pitfalls when initializing these arrays?**
Three common bugs often happen here:
1. **Size Mismatches**: Mixing up `num_partitions` and `mem_limit_records`. The buffer size is `mem_limit_records`, but the file and path arrays must be sized by `num_partitions`.
2. **Garbage Counters**: Using `malloc` for `partition_counts`. If you increment `counts[p]++`, it will start from a random garbage memory address! Always use `calloc` for counters so they strictly start at `0`.
3. **2D String Arrays**: Allocating `sizeof(char *) * num_partitions` only allocates an array of *pointers*. You still must loop through and allocate/format the actual string buffers for the file paths (e.g., `"%s/part_%zu.bin", temp_dir, i`).

**Q: Are there pitfalls during the teardown in `_destroy`?**
Yes! The most dangerous bug in a destructor is the **Use-After-Free (UAF)**. If you `free()` your metadata arrays (like `partition_files` or `partition_paths`) *before* you loop through them to close and delete the temporary files, your loop will read from freed memory, resulting in undefined behavior or a crash. 
Always perform your cleanup logic (closing files with `fclose`, deleting them with `remove()`) *before* you free the backing arrays! Furthermore, always check if a `FILE*` is non-NULL before passing it to `fclose`.

**Q: Wait, do I *really* need to allocate every single string in a 2D string array?**
Yes! This is a classic C memory concept. In Python or Java, strings are managed objects. But in C, `char **partition_paths` is just a "pointer to a pointer." 
When you do `malloc(sizeof(char *) * num_partitions)`, you are only asking the OS for a block of memory to hold $N$ memory addresses. However, those addresses initially point to random garbage. If you try to run `snprintf(partition_paths[0], ...)` without allocating the inner string, C will attempt to write text to a garbage memory address and immediately trigger a **Segmentation Fault**. 
You must explicitly tell C: "Here is the array of pointers, and by the way, make each pointer point to a brand new 256-byte block of memory where I can safely write my file path."

**Q: Can we optimize this by allocating one giant flat buffer (`256 * num_partitions`) instead of calling `malloc` in a loop?**
Absolutely! This is a brilliant systems-programming optimization called **Arena Allocation** or **Flat Allocation**. Calling `malloc` hundreds of times fragments the heap and incurs heavy syscall overhead. 
Instead, you can allocate the entire string storage in one contiguous block of memory, and then manually slice it up by pointing the `char **` array into specific offsets of that flat block:
```c
// 1. Allocate the array of pointers
agg->partition_paths = malloc(sizeof(char *) * num_partitions);

// 2. Allocate ALL string memory in one single contiguous block
char *flat_string_buffer = malloc(256 * num_partitions);

// 3. Map the pointers to offsets within the flat block
for (size_t i = 0; i < num_partitions; i++) {
    agg->partition_paths[i] = flat_string_buffer + (i * 256);
}
```
**Q: In `_push`, should I spill records one by one as they arrive?**
No! A high-performance database never writes to disk one row at a time. The entire purpose of `in_mem_buf` is to batch rows together. 

Here are the exact logical steps for the `_push` write path:
1. **Outer Loop**: Iterate through the incoming `records` array (from `i = 0` to `count`).
2. **Buffer**: Copy `records[i]` into `agg->in_mem_buf[agg->in_mem_count]` and increment `in_mem_count`.
3. **Check Limit**: If `agg->in_mem_count == agg->mem_limit_records`, the buffer is full and a **Bulk Spill** is triggered.
4. **The Bulk Spill (Inner Loop)**:
   - Iterate over the *buffered* records (from `j = 0` to `in_mem_count`).
   - For each buffered record, calculate its partition: `uint32_t p = agg->in_mem_buf[j].group_key % agg->num_partitions`.
   - If `agg->partition_files[p]` is `NULL`, lazily open the file: `fopen(agg->partition_paths[p], "ab")`.
   - Write the single buffered record: `fwrite(&agg->in_mem_buf[j], sizeof(agg_input_record_t), 1, agg->partition_files[p])`.
   - Increment `agg->partition_counts[p]`.
5. **Reset**: After the inner spill loop completes, set `agg->in_mem_count = 0`. The outer loop then continues processing any remaining incoming records.

*Note: During this Phase 1 ingestion, do not attempt to build or access the hash table (`current_hash_table`)! Phase 1 is strictly for routing raw records to disk partitions.*

**Q: Why do functions like `_push` and `_finish` return `int`, while `_next` returns `bool`?**
This follows classic POSIX/C conventions for API design:
- **Returning `int` (Action/Status)**: Used for functions that perform an action that could fail for multiple reasons (e.g., Out Of Memory, Disk Full, I/O Error). By convention, `0` means `SUCCESS`, and any non-zero value (often negative) indicates a specific error code. `_push` and `_finish` perform disk I/O, which can fail!
- **Returning `bool` (Question/Iterator)**: Used for functions that answer a binary question. For an iterator like `_next`, `true` means "I successfully yielded the next record", and `false` means "EOF (End of File) - there are no more records".

**Q: What is the exact intent of `_finish`? Why can't `_push` just do everything?**
`_push` is called repeatedly while streaming data, but `_push` only spills when the buffer is *100% full*. When the query engine reaches the end of the input table, there are usually leftover records still sitting in `in_mem_buf` (e.g., the buffer has 450 records, but the limit is 500). If we don't flush them, those records are lost forever!
The intent of `_finish` is to seal Phase 1:
1. **Flush the Leftovers**: It executes the exact same bulk-spill loop as `_push`, but only for the `in_mem_count` records remaining in the buffer.
2. **Close for Reading**: It loops through `partition_files` and calls `fclose` on any open files. In Phase 1 we opened them in `"ab"` (append) mode. To read them in Phase 2, we must close them so the OS flushes them to disk, allowing Phase 2 to safely reopen them in `"rb"` (read) mode!
3. **Lock the State**: It sets `agg->finished_ingestion = true` to signal the state machine can transition to Phase 2.

**Q: Doesn't this mean if we aggregate a massive amount of data, we write a massive amount of temporary files to disk?**
Yes! This is the fundamental physical limitation of Out-of-Core algorithms, known as **Write Amplification**. If you aggregate 100GB of data, you write 100GB to disk, just to read it back seconds later. 
Real databases mitigate this bottleneck using two techniques:
1. **Narrow Projections**: We never spill wide rows. Notice our `agg_input_record_t` is only 8 bytes (`int32_t` key, `int32_t` value). Even 1 billion rows will only write 8 GB to disk!
2. **Partial Pre-Aggregation**: Before executing the inner spill loop, a production database will attempt to aggregate duplicates *inside* `in_mem_buf`. If the 10,000 buffered rows only contain 50 unique keys, it aggregates them in memory and only spills 50 rows to disk! This drastically slashes disk I/O.

---

### Implementation Guide: Building Phase 2 (Aggregation & Merging)

Phase 1 successfully routed all incoming records to strictly segregated disk partitions. Now, the state machine transitions to Phase 2.

**The Intent of `_next`**
The query execution pipeline operates as a pull-based iterator. It will call `while (grace_hash_agg_next(agg, &out_record))` repeatedly until it returns `false`. 
The intent of `_next` is to yield one fully aggregated row (a `group_key` with its final `sum` and `count`) per call. 

Because we used Grace Hash partitioning, we have a mathematical guarantee: **all records with the same `group_key` are located in the exact same partition file**. Therefore, we never need to load the entire dataset into memory! We only process one partition at a time.

**The `_next` State Machine:**
1. **Load a Partition**: If `current_hash_table` is empty, check `current_partition_idx`. If we have exhausted all `num_partitions`, return `false` (EOF). Otherwise, open `partition_paths[current_partition_idx]` in `"rb"` (read) mode.
2. **Build the In-Memory Hash Table**: Read all records from the file. Insert each record into `current_hash_table`. If the `group_key` already exists, update its `sum_value` and `count_value`. If it's new, insert it.
3. **Yield**: Once the hash table is built (or if it was already built on a previous call), yield the record at `hash_table_iter_idx`, increment the iterator, and return `true`.
4. **Advance**: When `hash_table_iter_idx` reaches the end of the hash table, free the hash table, increment `current_partition_idx`, and on the *next* call to `_next`, the state machine will automatically load the next partition!

**Conceptual Logic Flow for `_next`:**
1. **Advance**: Check if a hash table currently exists in memory and if its iterator has reached its size. If so, free the hash table, set it to NULL, and increment the partition index.
2. **Skip Empties**: If the hash table is NULL, use a `while` loop to advance the partition index past any partitions that have a count of 0 (empty partitions).
3. **EOF Check**: If the partition index has reached the total number of partitions, return `false`—the query is completely finished!
4. **Build**: If the hash table is NULL (and we found a valid partition file with data), open that specific partition file in read mode. Use a `while` loop to read every single `agg_input_record_t` from the file. Hash each record into a dynamically sized array of `agg_output_record_t`. If it exists, update sum and count. If not, insert it with a count of 1. When the file is empty, close the file.
5. **Yield**: At the very end of the function (outside all the above `if` blocks), copy the record at the current iterator index into `out_record`, increment the iterator, and return `true`.

**Q: The nested loops in `_next` make the code hard to read. How do we improve C readability?**
When C code suffers from the "Arrow Anti-Pattern" (code that is indented so deeply it looks like `>`), the best solution is to extract the inner logic into a `static` helper function. 
In `_next`, the logic to search the hash table, resize it, and insert a new record takes up 20 lines inside a `while` loop! You should extract this into a helper function: 
`static void insert_or_update(grace_hash_agg_t *agg, agg_input_record_t in);`
This reduces the `fread` loop to a single, beautiful line of code, vastly improving the readability of the state machine. 
*(Additionally, always run `clang-format -i src/grace_hash_agg.c` to automatically fix messy indentation!)*

---

### The Final Listing

Here is the complete, working implementation of our Grace Hash Aggregation operator (`src/grace_hash_agg.c`).

```c
#include "simpledb/grace_hash_agg.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signal.h>

grace_hash_agg_t *grace_hash_agg_create(const char *temp_dir, size_t mem_limit_records,
                                        size_t num_partitions) {
    grace_hash_agg_t *agg = malloc(sizeof(*agg));

    agg->in_mem_count = 0;
    agg->mem_limit_records = mem_limit_records;
    agg->in_mem_buf = malloc(sizeof(agg_input_record_t) * mem_limit_records);
    if (!agg->in_mem_buf) {
        free(agg);
        return NULL;
    }

    agg->partition_counts = calloc(sizeof(size_t), num_partitions);
    if (!agg->partition_counts) {
        free(agg->in_mem_buf);
        free(agg);
        return NULL;
    }

    agg->partition_files = malloc(sizeof(FILE *) * num_partitions);
    if (!agg->partition_files) {
        free(agg->in_mem_buf);
        free(agg->partition_counts);
        free(agg);
        return NULL;
    }

    agg->partition_paths = malloc(sizeof(char *) * num_partitions);
    if (!agg->partition_paths) {
        free(agg->in_mem_buf);
        free(agg->partition_counts);
        free(agg->partition_files);
        free(agg);
        return NULL;
    }
    char *flat_buffer = calloc(sizeof(char), 256 * num_partitions);
    for (size_t i = 0; i < num_partitions; i++) {
        agg->partition_paths[i] = flat_buffer + (i * 256);
        snprintf(agg->partition_paths[i], 256, "%s/part_%zu.bin", temp_dir, i);
    }

    agg->num_partitions = num_partitions;
    snprintf(agg->temp_dir, sizeof(agg->temp_dir), "%s", temp_dir);

    return agg;
}

void grace_hash_agg_destroy(grace_hash_agg_t *agg) {
    if (!agg)
        return;

    free(agg->partition_counts);
    free(agg->in_mem_buf);

    for (size_t i = 0; i < agg->num_partitions; i++) {
        if (agg->partition_files[i]) {
            fclose(agg->partition_files[i]);
            remove(agg->partition_paths[i]);
        }
    }
    free(agg->partition_files);
    free(agg->partition_paths[0]);
    free(agg->partition_paths);

    free(agg);
}

int grace_hash_agg_push(grace_hash_agg_t *agg, const agg_input_record_t *records, size_t count) {
    for (size_t i = 0; i < count; i++) {
        agg->in_mem_buf[agg->in_mem_count++] = records[i];

        if (agg->in_mem_count == agg->mem_limit_records) {
            for (size_t j = 0; j < agg->mem_limit_records; j++) {
                uint32_t p = agg->in_mem_buf[j].group_key % agg->num_partitions;
                if (agg->partition_files[p] == NULL) {
                    // new file
                    agg->partition_files[p] = fopen(agg->partition_paths[p], "ab");
                    if (!agg->partition_files[p]) {
                        return -1;
                    }
                }
                fwrite(&agg->in_mem_buf[j], sizeof(agg_input_record_t), 1, agg->partition_files[p]);
                agg->partition_counts[p]++;
            }
            agg->in_mem_count = 0;
        }
    }
    return 0;
}

int grace_hash_agg_finish(grace_hash_agg_t *agg) {
    for (size_t j = 0; j < agg->in_mem_count; j++) {
        uint32_t p = agg->in_mem_buf[j].group_key % agg->num_partitions;
        if (agg->partition_files[p] == NULL) {
            // new file
            agg->partition_files[p] = fopen(agg->partition_paths[p], "ab");
            if (!agg->partition_files[p]) {
                return -1;
            }
        }
        fwrite(&agg->in_mem_buf[j], sizeof(agg_input_record_t), 1, agg->partition_files[p]);
        agg->partition_counts[p]++;
    }

    for (size_t p = 0; p < agg->num_partitions; p++) {
        if (agg->partition_files[p] != NULL) {
            fclose(agg->partition_files[p]);
        }
    }
    agg->finished_ingestion = true;

    return 0;
}

bool grace_hash_agg_next(grace_hash_agg_t *agg, agg_output_record_t *out_record) {
    if (!agg) {
        return false;
    }

    while (agg->hash_table_iter_idx >= agg->hash_table_size || !agg->current_hash_table) {
        if (agg->current_partition_idx >= agg->num_partitions)
            return false;
        // reset state
        free(agg->current_hash_table);
        agg->current_hash_table = NULL;
        agg->hash_table_size = 0;
        agg->hash_table_iter_idx = 0;
        // next partition
        agg_input_record_t in;
        agg->partition_files[agg->current_partition_idx] =
            fopen(agg->partition_paths[agg->current_partition_idx], "rb");
        while (fread(&in, sizeof(in), 1, agg->partition_files[agg->current_partition_idx]) == 1) {
            bool exists = false;
            for (size_t i = 0; i < agg->hash_table_size; i++) {
                if (agg->current_hash_table[i].group_key == in.group_key) {
                    exists = true;
                    agg->current_hash_table[i].count_value++;
                    agg->current_hash_table[i].sum_value += in.agg_value;
                }
            }
            if (!exists) {
                if (agg->hash_table_size >= agg->hash_table_capacity) {
                    // resize
                    agg->hash_table_capacity =
                        (agg->hash_table_capacity > 0) ? agg->hash_table_capacity * 2 : 16;
                    agg->current_hash_table = realloc(
                        agg->current_hash_table, sizeof(*out_record) * agg->hash_table_capacity);
                }

                agg_output_record_t new = {
                    .count_value = 1, .sum_value = in.agg_value, .group_key = in.group_key};
                agg->current_hash_table[agg->hash_table_size++] = new;
            }
        };
        fclose(agg->partition_files[agg->current_partition_idx]);
        agg->current_partition_idx++;

    }

    *out_record = agg->current_hash_table[agg->hash_table_iter_idx++];
    return true;
}
```
