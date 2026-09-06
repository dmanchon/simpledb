# Day 15: External Merge Sort (Disk-Spilling ORDER BY)

### Architectural Overview

Analytical queries frequently execute queries with `ORDER BY key LIMIT N` or require sorted streams for merge joins and window functions. When data volume exceeds available RAM, in-memory sorting algorithms (such as `qsort`, TimSort, or introsort) fail catastrophically—either triggering the OS Out-Of-Memory (OOM) killer or thrashing virtual memory swap spaces.

To process arbitrarily large datasets under a strict, deterministic memory budget, database engines implement **External Merge Sort**.

```text
                                INGESTION PHASE (Run Generation)
  Incoming Records Stream ──► [ Fixed RAM Buffer (M tuples) ]
                                            │
                                            │ (Buffer Full -> qsort in RAM)
                                            ▼
                              ┌─────────────────────────────┐
                              │ Sequential Write to Disk    │
                              └──────────────┬──────────────┘
                                             ▼
                 ┌───────────────────────────────────────────────────────┐
                 │ Spilled Sorted Runs:                                  │
                 │ [ run_0.bin ]   [ run_1.bin ]   ...   [ run_K-1.bin ] │
                 └───────────────────────────┬───────────────────────────┘
                                             │
                                STREAMING MERGE PHASE (K-Way Min-Heap)
                                             │
                         ┌───────────────────┴───────────────────┐
                         │ Open K file handles (1 item per run)  │
                         └───────────────────┬───────────────────┘
                                             ▼
                               ┌───────────────────────────┐
                               │  Min-Heap Priority Queue  │
                               │        (Size = K)         │
                               └─────────────┬─────────────┘
                                             │ Pop Min & Replenish
                                             ▼
                               Stream Sorted Records to Client
```

---

### Conceptual Comparison: `table_flush` vs. External Sort Disk Spilling

A natural question arises: *How does disk spilling in external sorting compare to `table_flush` from Day 14?*

Both mechanisms share the fundamental systems principle of **batched sequential disk I/O** to avoid random disk access:

| Characteristic | `table_flush` (Day 14 - Storage Layer) | External Sorter Spill (Day 15 - Execution Layer) |
| :--- | :--- | :--- |
| **Purpose** | Persisting new ingested rows permanently to disk. | Sorting records that exceed RAM budget during `ORDER BY`. |
| **Data Format** | Columnar format (`.bin` files, `.mrk` mark offsets, `primary.idx`). | Contiguous binary stream of sorted tuples (`sort_record_t`). |
| **Lifetime** | **Permanent / Immutable**: Remains on disk as an active table part. | **Ephemeral / Temporary**: Run files are deleted upon query completion (`external_sorter_destroy`). |
| **Read Path** | Scanned asynchronously via `column_reader_t` and Sparse Index. | Streamed immediately through a $K$-way **Min-Heap Priority Queue**. |

---

### The Two-Phase External Sort Algorithm

External Merge Sort operates in two decoupled phases:

#### 1. Phase 1: In-Memory Run Generation (Sort & Spill)
* Incoming records (`sort_record_t`) are appended to an in-memory buffer of fixed capacity `mem_limit_records`.
* Once the buffer reaches `mem_limit_records`, the engine sorts the in-memory array using standard in-place sorting (`qsort`).
* The sorted buffer is sequentially written out to a temporary run file on disk (e.g., `temp_dir/run_0000.run`), and the memory buffer counter is reset to zero.
* If the total record count never exceeds `mem_limit_records`, no disk I/O occurs—the sorter operates purely in RAM.

#### 2. Phase 2: $K$-Way Streaming Merge via Min-Heap
* When ingestion completes (`external_sorter_finish`), the sorter opens file handles to all $K$ spilled run files.
* A **Min-Heap** of capacity $K$ is initialized. The first record from each of the $K$ run files is pushed into the heap:
$$\text{Heap Node} = \{ \text{record} = (key, value), \text{run\_idx} = i \}$$
* On each call to `external_sorter_next()`:
  1. The root of the min-heap (the minimum key across all active runs) is popped and returned to the caller.
  2. The next record from that specific run file (`run_idx`) is read from disk and inserted into the heap.
  3. If a run file reaches EOF, the heap size shrinks by 1.
  4. The merge terminates when the heap is empty.

This produces a globally sorted stream in $O(N \log K)$ time while consuming only $O(K)$ active memory during the merge phase!

---

### C Header (`include/simpledb/external_sort.h`)

```c
#ifndef SIMPLEDB_EXTERNAL_SORT_H
#define SIMPLEDB_EXTERNAL_SORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// A sort record / tuple holding a 32-bit integer sort key and a 32-bit value / row_id
typedef struct {
    int32_t key;
    int32_t value;
} sort_record_t;

// A heap node used during K-way merge
typedef struct {
    sort_record_t record;
    size_t run_idx;
} heap_node_t;

// Min-heap structure for K-way merge
typedef struct {
    heap_node_t *data;
    size_t size;
    size_t capacity;
} min_heap_t;

// External sorter state
typedef struct external_sorter_t {
    char temp_dir[256];
    size_t mem_limit_records;  // Max records allowed in RAM buffer before spilling
    sort_record_t *in_mem_buf; // RAM buffer for run generation
    size_t in_mem_count;       // Current count in RAM buffer
    
    char **run_paths;          // Array of filepaths for temporary runs
    FILE **run_files;          // Open FILE pointers during merge phase
    size_t num_runs;           // Total number of spilled runs
    size_t run_paths_capacity;
    
    bool finished_ingestion;   // True after external_sorter_finish() is called
    min_heap_t heap;           // Priority queue for K-way merge
    size_t in_mem_iter_idx;    // If all records fit in RAM (0 runs spilled), iterator offset
} external_sorter_t;

// Create and destroy sorter
external_sorter_t *external_sorter_create(const char *temp_dir, size_t mem_limit_records);
void external_sorter_destroy(external_sorter_t *sorter);

// Push a batch of records into the sorter (spills to disk if RAM buffer fills up)
int external_sorter_push(external_sorter_t *sorter, const sort_record_t *records, size_t count);

// Seal ingestion and prepare for streaming sorted output
int external_sorter_finish(external_sorter_t *sorter);

// Pull next lowest record from the K-way merge heap (returns true if record found, false on EOF)
bool external_sorter_next(external_sorter_t *sorter, sort_record_t *out_record);

#endif // SIMPLEDB_EXTERNAL_SORT_H
```

---

### End-to-End Verification (`tests/test_db.c`)

```c
void test_external_sorter_in_memory(void) {
    printf("Testing External Sorter (In-Memory Fallback)...\n");

    external_sorter_t *sorter = external_sorter_create(".", 20);
    assert(sorter != NULL);

    sort_record_t input[5] = {
        { 40, 1 }, { 10, 2 }, { 50, 3 }, { 20, 4 }, { 30, 5 }
    };
    assert(external_sorter_push(sorter, input, 5) == 0);
    assert(external_sorter_finish(sorter) == 0);

    sort_record_t rec;
    int32_t expected_keys[5] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; i++) {
        assert(external_sorter_next(sorter, &rec) == true);
        assert(rec.key == expected_keys[i]);
    }
    assert(external_sorter_next(sorter, &rec) == false);

    external_sorter_destroy(sorter);
    printf("External Sorter In-Memory tests passed!\n");
}

void test_external_sorter_disk_spilling(void) {
    printf("Testing External Sorter (Disk-Spilling K-Way Merge)...\n");

    // Memory limit of 100 records -> 1000 items will trigger 10 spilled runs
    size_t mem_limit = 100;
    size_t total_records = 1000;
    external_sorter_t *sorter = external_sorter_create(".", mem_limit);
    assert(sorter != NULL);

    sort_record_t *input = malloc(sizeof(sort_record_t) * total_records);
    for (size_t i = 0; i < total_records; i++) {
        input[i].key = (int32_t)(total_records - i); // Reverse sorted keys
        input[i].value = (int32_t)i;
    }

    // Ingest in chunks of 50
    for (size_t i = 0; i < total_records; i += 50) {
        assert(external_sorter_push(sorter, input + i, 50) == 0);
    }
    free(input);

    assert(external_sorter_finish(sorter) == 0);
    assert(sorter->num_runs == 10);

    // Verify streaming sorted output from K-way merge
    sort_record_t rec;
    int32_t prev_key = -1;
    size_t count = 0;

    while (external_sorter_next(sorter, &rec)) {
        if (count > 0) {
            assert(rec.key >= prev_key);
        }
        prev_key = rec.key;
        count++;
    }

    assert(count == total_records);
    assert(prev_key == (int32_t)total_records);

    external_sorter_destroy(sorter);
    printf("External Sorter Disk-Spilling tests passed!\n");
}
```
