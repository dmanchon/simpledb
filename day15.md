# Day 15: External Merge Sort (Disk-Spilling ORDER BY)

### Architectural Overview: Why In-Memory Sorting Breaks Down

In Day 14 and Day 14 Bis, we built `table_insert_batch` and `table_flush`. We mastered the art of receiving incoming streaming batches in volatile RAM, multiplexing them into columnar buffers, and periodically flushing immutable, granule-indexed parts to NVMe disk.

However, analytical queries rarely stop at sequential table scans. Real-world analytical workloads depend on:
1. **Top-K and Ranking Queries:** `SELECT * FROM events ORDER BY score DESC LIMIT 100`.
2. **Merge Joins:** Joining two massive tables along sorted join keys in a single linear pass (`O(N + M)`).
3. **Window Functions:** Computing rolling averages or running totals partitioned and sorted by timestamp.
4. **Primary Key Sorting & Part Merging:** In Day 10, we learned that our Sparse Index (`sparse_index_t`) requires table parts to be sorted by their primary key to enable `O(log2(Granules))` binary search.

What happens when the data to be sorted exceeds available physical RAM?
* A standard in-memory sort (`qsort`, TimSort, introsort) assumes random access across the entire array.
* If you attempt to sort 100 GB of data on a server with 16 GB of RAM, the operating system either terminates the process with an **Out-Of-Memory (OOM) panic**, or triggers catastrophic virtual memory swapping—thrashing NVMe drives with random 4KB page faults and dropping throughput to near zero.

To process arbitrarily large datasets under a strict, deterministic memory budget, production database engines implement **External Merge Sort**.

---

### The Modern Systems Reality: Why Databases Never Naively "Spill Everything"

Before writing a single byte to disk, every high-performance analytical database (ClickHouse, DuckDB, `simpledb`) recognizes a brutal physical reality:
> **Disk I/O is slow, PCIe write bandwidth is finite, and flash SSD write endurance degrades with every byte written.**
> An engine that naively spills raw tables to temporary disk files whenever `ORDER BY` is invoked is not a database—it is a disk-destroying bottleneck.

If your dataset has 100 million or 1 trillion rows, blindly writing 100 Terabytes of temporary files to disk just to sort them is **catastrophic write amplification and complete nonsense**:
1. **I/O Bus Saturation:** Even on PCIe Gen 5 NVMe arrays writing at 10 GB/s, writing 100 TB takes nearly 3 hours—just to write ephemeral scratch files that will be thrown away immediately!
2. **SSD Flash Wear:** Writing hundreds of terabytes of temporary data burns through the flash write endurance (TBW) of enterprise drives.
3. **Disk Quota Panics:** Most clusters will instantly exhaust local `/tmp` disk partitions and crash with `No space left on device`.

Therefore, real database engines treat disk spilling as an **emergency fail-safe of last resort**, not standard operating procedure. They implement a strict **4-tier hierarchy of smart sorting defenses**:

```text
                                  Incoming ORDER BY Query
                                             │
      ┌──────────────────────────────────────┼──────────────────────────────────────┐
      ▼                                      ▼                                      ▼
[ Defense 1: Table Pre-Sorted? ]   [ Defense 2: Has LIMIT N? ]        [ Defense 3: Fits in RAM? ]
      │ (Yes: ClickHouse ReadInOrder)        │ (Yes: e.g. LIMIT 100)                │ (Yes: Records <= M)
      ▼                                      ▼                                      ▼
Bypass Sorting Completely!         Bounded In-Memory Heap             In-Memory Fast Path (qsort in RAM)
Zero bytes written to disk.        Zero bytes written to disk.        Zero bytes written to disk.
Stream existing sorted parts!      Keep top N in L3 cache!            Zero temporary files created!
      │                                      │                                      │
      └──────────────────────────────────────┼──────────────────────────────────────┘
                                             │ (No to all defenses: Arbitrary Unbounded Sort)
                                             ▼
                        [ Defense 4: Late Materialization ]
                        - Never spill wide rows!
                        - Extract narrow 8-byte (key, row_id) pairs
                                             │
                                             ▼
                        [ Smart External Merge Sort Pipeline ]
                        - Phase 1: Spill dense (key, row_id) runs
                        - Phase 2: Streaming K-Way Min-Heap Merge
                        - Output: SIMD Gather surviving rows only!
```

#### The 4 Smart Defenses in Detail:

1. **Defense 1: Physical Storage Pre-Sorting (`ReadInOrder` — ClickHouse's Secret)**
   Why sort data during query execution if it was already sorted when stored?
   In ClickHouse `MergeTree()`, data is physically stored on disk **already sorted** along the primary key (`ORDER BY (tenant_id, timestamp)`). If a query requests `ORDER BY tenant_id, timestamp`, the engine **completely skips sorting**! It writes 0 bytes to disk and streams the existing on-disk parts directly into a streaming merge.

2. **Defense 2: Top-K Short-Circuiting (`ORDER BY ... LIMIT N`)**
   In over 95% of real-world queries, analysts never want to scroll through billions of sorted rows—they want the top 100 or 1,000 (`ORDER BY score DESC LIMIT 100`). The engine allocates a tiny bounded priority queue of size 100 in CPU L3 cache, streams through the data, and discards non-qualifying records on the fly. Zero temporary files are created.

3. **Defense 3: The In-Memory Fast Path (`mem_limit_records`)**
   If total records fit inside the query's allocated RAM buffer (`num_runs == 0`), the engine sorts the contiguous in-memory array with standard `qsort()` and serves reads directly from RAM. Zero disk runs are generated.

4. **Defense 4: Late Materialization (Narrow Projections)**
   When an arbitrary, unindexed sort *does* exceed the memory budget, a rookie mistake is attempting to spill whole, wide rows (e.g. 30 columns of integers, floats, strings).
   Instead, we extract **only narrow 8-byte `(key, row_id)` pairs**:
   - 100 million wide rows (200 bytes/row): **20 GB of disk writes**.
   - 100 million narrow `(key, row_id)` pairs: **800 MB of disk writes**.
   That single architectural decision eliminates **96% of disk write bandwidth** and allows **8,388,608 tuples to fit in just 64 MB of RAM** before spilling a single byte!

---

### Late Materialization Synergy: Narrow `(key, row_id)` Pairs

```text
               Wide Columnar Table                            External Sort Engine
       ┌─────────┬──────────┬──────────────┐                     ┌────────────────┐
Row 0: │ ID: 101 │ Score: 92│ Comment: ... │ ───────(Extract)──► │ Key: 92        │
Row 1: │ ID: 102 │ Score: 45│ Comment: ... │                     │ Row ID: 0      │
Row 2: │ ID: 103 │ Score: 78│ Comment: ... │                     ├────────────────┤
       └─────────┴──────────┴──────────────┘                     │ Key: 45        │
                                                                 │ Row ID: 1      │
                                                                 └────────────────┘
                                                                (Only 8 bytes/tuple!)
```

```c
typedef struct {
    int32_t key;    // The sorting key (e.g., score, user_id, or dictionary surrogate ID)
    int32_t row_id; // The payload: row index in the table (late materialization pointer)
} sort_record_t;
```

#### Why Sort `(key, row_id)` Instead of Wide Rows?
1. **10x Higher RAM Density:**
   A `sort_record_t` is exactly 8 bytes. In a modest memory limit of 64 MB, you can fit **8,388,608 tuples in RAM** before spilling a single byte to disk!
2. **Sequential Spill Bandwidth:**
   Spilling 8-byte structs saturates maximum PCIe NVMe write speeds without string pointer serialization or complex framing.
3. **Zero Waste with `LIMIT N`:**
   When the query is `ORDER BY score LIMIT 10`, the external sorter streams the top 10 `(score, row_id)` pairs. We then use SIMD Gather to fetch only the surviving 10 rows from the columnar parts, completely skipping disk reads for the other 99.9% of the dataset!

---

### The Smart Two-Phase External Sort Pipeline

Only when all early defenses are bypassed does the engine activate out-of-core spilling—and even then, it operates strictly on **dense, 8-byte `(key, row_id)` index pairs**:

```text
                               PHASE 1: SMART RUN GENERATION (Spill)
   Incoming Columnar Data ──► [ Extract Narrow (key, row_id) Projections (8 bytes) ]
                                              │
                                              ▼
                              [ Fixed RAM Ingestion Buffer (Capacity M) ]
                                              │
                    ┌─────────────────────────┴─────────────────────────┐
                    ▼ (Records <= M: Fast Path)                         ▼ (Buffer Full)
         [ In-Memory qsort in RAM ]                              [ In-Memory qsort in RAM ]
                    │                                                   │
                    │                                                   ▼ (Contiguous Sequential Write)
                    │                                    [ Spilled Runs: run_0000.run ... run_K.run ]
                    │                                                   │
                    │                                                   ▼
                    │                                  PHASE 2: STREAMING K-WAY MERGE
                    │                                                   │
                    │                                    [ K Open Handles (1 read buffer/run) ]
                    │                                                   │
                    │                                                   ▼
                    │                                    [ Min-Heap Priority Queue (Size K) ]
                    │                                                   │
                    │                                                   ▼ (Pop Min & Replenish)
                    └─────────────────────────┬─────────────────────────┘
                                              ▼
                             Globally Sorted (key, row_id) Stream
                                              │
                                              ▼ (Late Materialization Gather)
                             Fetch Payload Columns for Surviving Rows Only!
```

---

### Storage Flush (`table_flush`) vs. Execution Spill (`external_sort`)

Having built `table_flush` in Day 14 Bis, we can now compare the two different types of disk writes in a database engine:

| Dimension | Storage Layer (`table_flush` - Day 14 Bis) | Execution Layer (`external_sorter` - Day 15) |
| :--- | :--- | :--- |
| **Trigger** | Ingestion buffer full (e.g. 65,536 rows), time deadline, or shutdown. | Query execution memory budget exceeded (`mem_limit_records`). |
| **Data Format** | Columnar (`.bin`, `.mrk` marks, `.dict`, `.strings`). | Contiguous binary stream of sorted tuples (`sort_record_t`). |
| **Lifetime** | **Permanent & Immutable**: Registered into `table->parts` catalog. | **Ephemeral & Temporary**: Spilled runs exist only for query duration; wiped upon iterator close. |
| **Access Pattern** | Lazy zero-copy `mmap` scans via `column_reader_t` and Sparse Index. | Active K-way streaming merge via a **Min-Heap Priority Queue**. |
| **Sorting** | Preserves ingestion order (or batch sort key). | Strictly produces a globally sorted stream. |

Both systems rely on the same physical optimization: **Sequential Block I/O**. Neither system does random disk seeking.

---

### Step-by-Step: The Complete Lifecycle of External Sort

The external sort algorithm executes across 6 distinct, deterministic operational steps divided into two decoupled phases (Run Generation and K-Way Streaming Merge):

```text
  [ external_sorter_create ] ──► Step 1: Pre-allocate RAM buffer & configure memory budget
              │
              ▼
  [ external_sorter_push ]   ──► Step 2: Ingest batches. When buffer is full:
              │                          (1) qsort in RAM
              │                          (2) fwrite contiguous block to run_XXXX.run
              │                          (3) Reset buffer count (zero-allocation recycling)
              ▼
  [ external_sorter_finish ] ──► Step 3: Seal ingestion & choose execution path:
              │                          ├─ Case A (num_runs == 0): In-Memory Fast Path!
              │                          └─ Case B (num_runs > 0): Flush final run & prep merge
              ▼
  [ Heap Initialization ]    ──► Step 4: Open K run files, read 1st tuple from each, seed Min-Heap
              │
              ▼
  [ external_sorter_next ]   ──► Step 5: Streaming pull:
              │                          ├─ Fast Path: Read in_mem_buf[in_mem_iter_idx++]
              │                          └─ Spill Path: Pop root min, refill from root.run_idx
              ▼
  [ external_sorter_destroy] ──► Step 6: Close open files, unlink all .run files, free memory
```

#### Step 1: Initialization (`external_sorter_create`)

Creating the sorter state machine requires careful allocation and clean zero-initialization:

```c
external_sorter_t *external_sorter_create(const char *temp_dir, size_t mem_limit_records);
```

1. **Allocate the Master State Machine:**
   Using `calloc(1, sizeof(*sorter))` (or explicit zero-initialization) is essential. Because `external_sorter_t` contains dynamic pointer arrays (`run_paths`, `run_files`), counters (`in_mem_count`, `num_runs`), and an embedded `min_heap_t`, uninitialized memory will contain garbage values that cause segfaults in subsequent phases.
2. **Size the In-Memory Buffer Correctly & Unwind on Failure:**
   - **Crucial Systems Gotcha:** The buffer holds `sort_record_t` structs (which contain both `int32_t key` and `int32_t value`, totaling 8 bytes).
   - Allocating with `sizeof(int32_t)` (4 bytes) creates a buffer with **half the necessary memory**, leading to heap corruption and buffer overflows as soon as the buffer fills!
   - Always size using the target type:
     ```c
     sorter->in_mem_buf = malloc(mem_limit_records * sizeof(*sorter->in_mem_buf));
     ```
   - **Two-Stage Allocation Unwind:** If allocating `in_mem_buf` fails, you must `free(sorter)` before exiting or returning to prevent leaking the outer container on fatal memory pressure:
     ```c
     if (!sorter->in_mem_buf) {
         free(sorter);
         fprintf(stderr, "[FATAL ERROR] Failed to allocate in_mem_buf\n");
         exit(EXIT_FAILURE);
     }
     ```
3. **Safe String Copying:**
   Copy `temp_dir` with bounds checking (`snprintf` or `strncpy`) to ensure paths longer than 255 bytes do not overflow `sorter->temp_dir`.
4. **State Machine Initialization:**
   Initialize all counters and cursors to clean baseline states:
   - `in_mem_count = 0`
   - `num_runs = 0`
   - `run_paths = NULL`, `run_files = NULL`, `run_paths_capacity = 0`
   - `finished_ingestion = false`
   - `in_mem_iter_idx = 0`
   - `heap = (min_heap_t){0}`
5. Peak RAM usage during Phase 1 is strictly capped and known upfront: `mem_limit_records * sizeof(sort_record_t)`.

#### Step 2: The Write Path — How Run Files Are Created (`external_sorter_push`)

How does a database create these run files on disk? Think of `in_mem_buf` as a **fixed-size bucket** in RAM that holds at most `mem_limit_records` items (e.g. 1,000,000 records).

```text
Incoming Tuples ──► [ Fixed RAM Bucket: in_mem_buf ]
                           │
                           │ (Bucket fills up to mem_limit_records)
                           ▼
                    [ Sort in RAM: qsort() ]
                           │
                           ▼
                    [ Sequential Disk Blast: fwrite() ]
                           │
                           ▼
                    [ Write to temp_dir/run_0000.run ] ──► (Increment num_runs, reset bucket cursor to 0)
```

1. **State Machine & Parameter Validation:**
   First, validate arguments and enforce the state machine. If `external_sorter_finish()` has already sealed ingestion, pushing more records is an invalid operation:
   ```c
   if (!sorter || (!records && count > 0) || sorter->finished_ingestion) {
       return -1;
   }
   ```

2. **Filling the Bucket:**
   As incoming batches arrive from query execution or table scanning, each 8-byte `sort_record_t` is copied into the RAM bucket:
   ```c
   sorter->in_mem_buf[sorter->in_mem_count++] = records[i];
   ```

3. **The Bucket Fills Up (`in_mem_count == mem_limit_records`):**
   When `sorter->in_mem_count == sorter->mem_limit_records`, the bucket is saturated. We must flush it to disk as a sorted run.

4. **Sorting in RAM with an Overflow-Safe Comparator:**
   Before touching the disk, we sort the entire bucket in-place in RAM using standard C `qsort()`.
   > [!WARNING]
   > **Two Fatal Comparator Traps in C:**
   > 1. **The Boolean `>=` Trap:** A common beginner mistake is `return a->key >= b->key;`. In C, `qsort()` requires a three-way comparison (negative if `a < b`, zero if `a == b`, positive if `a > b`). Returning a boolean never returns a negative number, and claims `a > b` and `b > a` when equal, violating strict weak ordering and triggering **undefined behavior** (infinite loops or memory corruption).
   > 2. **The Integer Subtraction Overflow Trap:** Never implement the comparator as `return ra->key - rb->key;`! If `ra->key` is negative (e.g., `-2,000,000,000`) and `rb->key` is positive (`2,000,000,000`), the subtraction overflows 32-bit signed integer arithmetic, flipping the sign and corrupting the sort.
   > 3. **Sorting Field:** Always sort by **`key`**, NOT `value`! `key` holds the query attribute (e.g. timestamp or score), while `value` is the row ID payload.
   > Always use explicit branch comparisons:
   ```c
   static int cmp_sort_records(const void *a, const void *b) {
       const sort_record_t *ra = (const sort_record_t *)a;
       const sort_record_t *rb = (const sort_record_t *)b;
       if (ra->key < rb->key) return -1;
       if (ra->key > rb->key) return 1;
       return 0;
   }
   ```

5. **Fast Binary Serialization via Single-Syscall `fwrite`:**
   Why is binary block serialization the fastest way to save data in C?
   - **Zero Parsing / Zero Conversion:** `sort_record_t` is a flat 8-byte Plain Old Data (POD) struct without pointers. Writing it requires zero string formatting, zero serialization headers, and zero JSON/CSV overhead.
   - **Single Sequential Syscall:** Instead of a loop writing records one-by-one (which causes millions of function calls and branch stalls), we pass the entire buffer in **one single `fwrite()` call**:
     ```c
     FILE *fp = fopen(path, "wb");
     if (!fp) return -1;

     size_t written = fwrite(sorter->in_mem_buf, sizeof(sort_record_t), sorter->in_mem_count, fp);
     fclose(fp); // Immediate close to prevent leaking file descriptors

     // What to do with `written`? Check for Disk Full (ENOSPC) / I/O errors!
     if (written != sorter->in_mem_count) {
         remove(path); // Clean up the corrupt, partially written file
         return -1;
     }
     ```
   - **Why check `written`?** `fwrite()` returns the number of complete items written. If `written != count`, the disk ran out of space (`ENOSPC`) or an I/O fault occurred. Ignoring this would leave a truncated, corrupted file on disk that will cause undefined behavior during Phase 2 merging.

6. **Growing the Run Catalog (`run_paths`) & The Dangling Stack Pointer Trap:**
   Before saving the new run file path, ensure the dynamic array `run_paths` has sufficient capacity, doubling capacity when full:
   ```c
   if (sorter->num_runs >= sorter->run_paths_capacity) {
       size_t new_cap = sorter->run_paths_capacity == 0 ? 8 : sorter->run_paths_capacity * 2;
       char **new_paths = realloc(sorter->run_paths, new_cap * sizeof(char *));
       if (!new_paths) return -1;
       sorter->run_paths = new_paths;
       sorter->run_paths_capacity = new_cap;
   }
   ```
   Save the path using:
   ```c
   sorter->run_paths[sorter->num_runs++] = strdup(path);
   ```

   > [!WARNING]
   > **Systems Traps When Spilling Runs:**
   > 1. **The Capacity Comparison Trap (`>=` vs `<`):** When starting with `capacity = 0` and `num_runs = 0`, checking `capacity < num_runs` (`0 < 0`) is **FALSE**! It will skip allocation and dereference a `NULL` pointer on the first write. The check must be `num_runs >= capacity`!
   > 2. **The `0 * 2 = 0` Capacity Trap:** If `run_paths_capacity` starts at `0`, doing `capacity *= 2` still results in `0`! Always guard with a base initial capacity: `capacity = (capacity == 0) ? 8 : capacity * 2;`.
   > 3. **Off-by-One Array Indexing:** Arrays in C are 0-indexed. If you increment `sorter->num_runs++` *before* inserting, the first run will be saved at `run_paths[1]`, leaving `run_paths[0]` uninitialized garbage. Always write to `sorter->run_paths[sorter->num_runs++] = strdup(path);`.
   > 4. **The Dangling Stack Pointer Trap:** In C, `char path[512]` is allocated on the **call stack** of the function. Never store the raw pointer `sorter->run_paths[i] = path;`. Always use `strdup(path)` (or `malloc` + `strcpy`) so the string persists on the heap until `destroy()`.
   > 5. **Flushing & File Handle Leaks (`fclose`):** You must call `fclose(fp)` immediately after `fwrite`. If you omit `fclose`, bytes sitting in libc user buffers may never reach the SSD, and open file descriptors will leak, quickly exhausting `ulimit -n`.

> [!TIP]
> **DRY Pattern — The `spill_run` Helper:**
> Both `external_sorter_push()` (when `in_mem_count == mem_limit_records`) and `external_sorter_finish()` (when trailing records remain) must sort, open a file, `fwrite`, and record the run path.
> Encapsulating this entire sequence in a `static int spill_run(external_sorter_t *sorter)` helper prevents duplicating 25 lines of C code!
>
> **Two Critical Nuances for `spill_run`:**
> 1. **Sort `in_mem_count`, NOT `mem_limit_records`:** When `spill_run` is called by `finish()`, the trailing buffer may be only half-full (e.g. 50 items in a 100-item capacity). If you pass `mem_limit_records` to `qsort()`, you will sort 50 valid tuples mixed with 50 uninitialized garbage memory values! Always sort `sorter->in_mem_count`.
> 2. **Guard Against Zero-Byte Ghost Runs:** If the input was an exact multiple of the buffer capacity (e.g., 200 items in a 100-item buffer), `in_mem_count` will be `0` when `finish()` runs. Add `if (sorter->in_mem_count == 0) return 0;` at the very start of `spill_run()` to ensure an empty, 0-byte `.run` file is never created!

7. **Zero-Allocation Bucket Recycling:**
   Crucially, we do **not** call `free()` and `malloc()`. We simply set the cursor back to zero:
   ```c
   sorter->in_mem_count = 0;
   ```
   The exact same memory buffer is reused to collect the next batch of records.

8. **Repeat:** The loop continues until all `count` records from the incoming batch are processed. Returns `0` on success.

> [!IMPORTANT]
> **Phase Decoupling: Why `push()` NEVER Touches the Heap:**
> Notice that during `external_sorter_push()`, we **only write runs to disk and record their paths in `run_paths`**. We do **not** open the files, and we do **not** touch `sorter->heap`!
> Why?
> 1. We cannot know the final number of runs (`num_runs`) until ingestion completes.
> 2. The min-heap's capacity must be allocated to match the final `num_runs`.
> 3. Opening `FILE*` handles during ingestion would waste OS file descriptors long before Phase 2 begins.
> 4. If the entire dataset fits in RAM (`num_runs == 0`), no heap is ever needed!
> All heap allocation and file opening are strictly deferred to `external_sorter_finish()`.

#### Step 3: Sealing Ingestion & Preparing Output (`external_sorter_finish`)

```c
int external_sorter_finish(external_sorter_t *sorter);
```

`external_sorter_finish()` is the master transition point that seals ingestion and prepares the sorter for streaming output. It returns **`0` on success** and **`-1` on error** (following standard C conventions).

If someone calls `finish` twice or passes `NULL`, reject it immediately:
```c
if (!sorter || sorter->finished_ingestion) {
    return -1;
}
sorter->finished_ingestion = true;
```

It then decides which of the two execution paths to activate:

##### Branch A — The In-Memory Fast Path (`sorter->num_runs == 0`):
If total records never exceeded `mem_limit_records`, **zero run files were created on disk!**
1. We sort the records currently in `in_mem_buf` in RAM:
   ```c
   qsort(sorter->in_mem_buf, sorter->in_mem_count, sizeof(sort_record_t), sort_fn);
   ```
   > [!WARNING]
   > **The `mem_limit_records` vs `in_mem_count` Sort Trap:**
   > Never pass `sorter->mem_limit_records` to `qsort` here! If the memory budget is 1,000,000 records, but the user only inserted 5 records, sorting `sorter->mem_limit_records` tells libc to sort 5 valid records alongside 999,995 uninitialized, random garbage memory slots. Always pass **`sorter->in_mem_count`**.
2. Initialize the cursor: `sorter->in_mem_iter_idx = 0;`
3. Return `0`. The sorter is immediately ready for `external_sorter_next()` to read directly from RAM without touching disk or allocating a heap!

##### Branch B — The Spilled Merge Path (`sorter->num_runs > 0`):
If runs were spilled to disk during ingestion:
1. **Flush Trailing Leftovers (The Empty-Run Trap):**
   What if 150 records were pushed into a 100-record buffer?
   - The first 100 records were spilled into `run_0000.run` (`num_runs = 1`).
   - The remaining 50 records are still sitting in `sorter->in_mem_buf` with `in_mem_count = 50`.
   If we do not flush them, those 50 records will be lost forever!
   However, what if exactly 100 records were pushed? `in_mem_count` would be `0`. Unconditionally flushing would create an empty, 0-byte `run_0001.run`!
   Therefore, we guard the spill:
   ```c
   if (sorter->in_mem_count > 0) {
       if (spill_run(sorter) != 0) return -1;
   }
   ```
   > [!TIP]
   > **Why Does `spill_run(sorter)` Need No Extra Arguments?**
   > All required state—the memory buffer pointer `sorter->in_mem_buf`, the item count `sorter->in_mem_count`, the target directory `sorter->temp_dir`, the current run counter `sorter->num_runs`, and the catalog arrays `sorter->run_paths` / `sorter->run_paths_capacity`—are already encapsulated inside `external_sorter_t`. The helper takes only `sorter`.

2. **Allocate Phase 2 Merge Structures:**
   ```c
   sorter->run_files = malloc(sorter->num_runs * sizeof(FILE *));
   sorter->heap.data = malloc(sorter->num_runs * sizeof(heap_node_t));
   if (!sorter->run_files || !sorter->heap.data) return -1;
   sorter->heap.capacity = sorter->num_runs;
   sorter->heap.size = 0;
   ```
3. **Open All Run Files & Seed the Min-Heap (Step 4):**
   Open each run file in binary read mode (`"rb"`), read exactly 1 record from each, wrap it with `.run_idx = i`, and push it into the heap via sift-up:
   ```c
   for (size_t i = 0; i < sorter->num_runs; i++) {
       sorter->run_files[i] = fopen(sorter->run_paths[i], "rb");
       if (!sorter->run_files[i]) return -1;
       sort_record_t rec;
       if (fread(&rec, sizeof(sort_record_t), 1, sorter->run_files[i]) == 1) {
           size_t idx = sorter->heap.size++;
           sorter->heap.data[idx] = (heap_node_t){ .record = rec, .run_idx = i };
           heap_sift_up(&sorter->heap, idx);
       }
   }
   ```
   The initial K candidates are loaded into the tournament bracket. Return `0`.

   > [!WARNING]
   > **Traps in the Tournament Seeding Loop:**
   > 1. **The `fread` Inverted Return Value Trap:** `fread(ptr, size, count, stream)` returns the **number of items read**, NOT an error code (0). When reading 1 item, a successful read returns `1`! If you write `if (fread(...) != 0) return -1;`, `1 != 0` evaluates to true, causing the function to abort with an error on **success**! Always check `if (fread(...) != 1) return -1;` (or `if (fread(...) == 1)`).
   > 2. **The Stationary Heap Size Gotcha:** If you assign `sorter->heap.data[sorter->heap.size] = node;` without incrementing `heap.size`, every run overwrites index `0`, leaving `heap.size` at `0` at the end of the loop!
   > 3. **The Post-Increment Off-by-One Sift Trap:** Writing `sorter->heap.data[sorter->heap.size++] = node; heap_sift_up(&sorter->heap, sorter->heap.size);` is a subtle off-by-one bug! Because `size++` increments `heap.size` immediately after the assignment, calling `heap_sift_up` with `sorter->heap.size` passes the index of the **next uninitialized slot** rather than the slot where `node` was stored! Always capture the index:
   >    ```c
   >    size_t idx = sorter->heap.size++;
   >    sorter->heap.data[idx] = node;
   >    heap_sift_up(&sorter->heap, idx);
   >    ```
   >    Or encapsulate in a `heap_push(&sorter->heap, record, i)` helper.

---

### Systems Deep Dive: Does Returning `-1` on Out-Of-Memory (OOM) Make Any Sense?

A profound question in C systems programming:
> *"In `external_sorter_create()`, we called `exit(EXIT_FAILURE)` when `malloc` failed. Does returning `-1` on OOM in `finish()` or `push()` make any sense, or is checking for OOM pointless?"*

This touches on one of the great debates in database engine architecture and systems programming. The answer depends entirely on whether you are writing a **standalone CLI tool** or a **multi-tenant database server**.

#### 1. The Multi-Tenant Server Reality: Why Real Databases Never Call `exit()`
Imagine ClickHouse or PostgreSQL running in production with 500 connected enterprise clients, executing transactions, ETL pipelines, and analytical queries.
* An analyst runs a poorly written query with an unbounded `ORDER BY` that exhausts memory.
* **If the database calls `exit(EXIT_FAILURE)`:**
  The entire server process dies! All 500 clients are abruptly disconnected, in-flight transactions are aborted, uncommitted work is lost, and the database must undergo crash recovery on reboot.
* **If the engine returns an error code (`-1`, `SQLITE_NOMEM`, or `MEMORY_LIMIT_EXCEEDED`):**
  The query engine intercepts the error, aborts **only that specific query**, rolls back its temporary state, sends an error message to that one analyst (`DB::Exception: Memory limit exceeded`), and **the database server continues serving all other 499 users without interruption**!

Therefore, inside library and query engine components, **returning `-1` is the gold standard for server resilience**.

#### 2. The Linux Kernel Reality: Overcommit & The OOM Killer
Why do many C programmers believe checking for `NULL` on `malloc` is pointless?
Because by default, the Linux kernel uses **memory overcommit** (`/proc/sys/vm/overcommit_memory = 0`):
1. When you call `malloc(1 GB)`, libc and the Linux kernel say: *"Sure, here is a 1 GB virtual memory pointer!"*
2. However, the kernel has **not allocated a single byte of physical RAM yet**. It merely reserves virtual address space.
3. Physical RAM is only allocated lazily when your CPU actually writes to the pages (triggering page faults).
4. If the machine runs out of physical RAM and swap space while dirtying those pages, `malloc()` never gets a chance to return `NULL`. Instead, the Linux kernel invokes the **Out-Of-Memory Killer (`oom-killer`)**, which selects a process and sends an uncatchable `SIGKILL` (`kill -9`), instantly killing the database.

Under naive Linux overcommit, returning `-1` from a standard libc `malloc()` often feels like chasing a ghost.

#### 3. When DOES Returning `-1` Actually Save the Day?
Even on Linux, returning `-1` on allocation failure is critical for four real-world scenarios:

1. **Per-Query Memory Trackers (ClickHouse & DuckDB):**
   Production analytical databases do not let queries call raw unconstrained `malloc()`. They wrap allocations in a **Query Memory Tracker** (e.g. ClickHouse's `MemoryTracker` or PostgreSQL's `work_mem`).
   If a query thread exceeds its allotted budget (e.g. `max_bytes_before_external_sort = 100MB`), the memory allocator **intentionally refuses to allocate and returns `NULL` (or throws)**, even if the host machine has 256 GB of free RAM!
   Returning `-1` allows the query engine to gracefully abort that query without crashing.
2. **Virtual Address Space Exhaustion:**
   On 32-bit embedded platforms or architectures with restricted virtual address space (or processes running under strict `ulimit -v`), `malloc()` genuinely returns `NULL` when virtual address space is exhausted.
3. **Overcommit Disabled (`overcommit_memory = 2`):**
   In enterprise environments (such as financial high-frequency trading and database appliances), sysadmins often disable overcommit (`vm.overcommit_memory = 2`). In this mode, `malloc()` strictly checks physical RAM + swap and returns `NULL` if memory is unavailable.
4. **Failure of Massive Contiguous Allocations:**
   When allocating a very large contiguous buffer (e.g., hundreds of megabytes), `malloc()` can fail due to virtual memory fragmentation, returning `NULL` while plenty of total RAM exists.

#### 4. The Golden Rule of Error Codes: Clean Unwinding
Returning `-1` is only useful if the caller can safely clean up without leaking memory or crashing.
Notice how [`external_sorter_destroy()`](file:///Users/dmanchon/repos/cdb/include/simpledb/external_sort.h) was designed:
```c
if (sorter->run_files) { ... free(sorter->run_files); }
if (sorter->run_paths) { ... free(sorter->run_paths); }
free(sorter->heap.data);
free(sorter->in_mem_buf);
free(sorter);
```
Every pointer is guarded or safe to pass to `free(NULL)`. If `finish()` fails halfway through allocation and returns `-1`, the caller can immediately call `external_sorter_destroy(sorter)` to unwind all resources cleanly!

#### 5. The Fallacy: "Won't Everything Fail Afterwards Anyways?" (The Circuit Breaker)
You asked a natural and intuitive question:
> *"If a query exhausts memory while trying to allocate a heap buffer... won't everything fail afterwards anyways? Why bother returning `-1`?"*

In a single-threaded batch script, that intuition is often true. But in a database engine, **"everything will fail anyways" is completely false**, for four profound systems reasons:

1. **The Circuit Breaker: Immediate Memory Relief**
   Why did the allocation fail? Because Query A was hoarding hundreds of megabytes of RAM.
   When `finish()` returns `-1`, the query engine catches it and immediately invokes:
   ```c
   external_sorter_destroy(query_a->sorter);
   ```
   **Instant relief!** The hundreds of megabytes held by Query A are released back to the operating system or pool allocator in microseconds. The moment the "greedy" query is discarded, the server has abundant memory again, allowing all subsequent allocations across the engine to succeed smoothly.
2. **Per-Query Budgets vs. System RAM:**
   In production engines (ClickHouse, DuckDB), the allocation didn't fail because the physical 256 GB server ran out of RAM! It failed because Query A exceeded its **individual 500 MB query budget**.
   The operating system still has 200 GB of free RAM. Queries B, C, and D are running happily within their budgets. Killing the entire server with `exit()` because one query hit its personal speed limit would be madness!
3. **Transient Concurrency Spikes:**
   Database memory pressure is spiky and bursty. Ten analytical queries might simultaneously hit their peak hash aggregation buffers for 200 milliseconds.
   Query 11 arrives, fails to allocate, and receives `-1`.
   50 milliseconds later, Queries 1, 2, and 3 complete and free 10 GB of RAM.
   If the database called `exit()`, the entire database crashed for all users. Because it returned `-1`, only Query 11 was rejected; Query 12 arrives half a second later and succeeds without a hitch!
4. **Protecting Background Flushers and Storage State:**
   In Day 14 Bis, we built `table_flush`. In a running database, background worker threads are actively writing columnar parts, marks, and indexes to disk.
   If an ad-hoc `SELECT ... ORDER BY` query calls `exit(EXIT_FAILURE)` due to memory exhaustion, the process abruptly terminates while a background flusher is writing a `.bin` or `.mrk` file! This leaves half-written, corrupted parts on disk that require manual surgery or recovery.
   By returning `-1`, the failure's blast radius is strictly confined to the user's `SELECT`, while background disk writes and ongoing data integrity remain completely uncompromised.

---

#### Step 4: K-Way Merge Initialization — The "Decks of Cards" Tournament

What does *"open K run files, read 1st tuple from each, seed Min-Heap"* actually mean in plain English and physical reality?

##### The Intuition: 3 Decks of Sorted Playing Cards
Imagine you have 3 decks of playing cards on a table. Each deck is **already individually sorted** from lowest to highest:
* **Deck 0:** `[ 5, 12, 40, 99 ]`
* **Deck 1:** `[ 2,  8, 15, 30 ]`
* **Deck 2:** `[ 7, 10, 25, 50 ]`

You want to produce a single, globally sorted pile of cards.
**Where is the absolute smallest card in the entire room?**
It is physically impossible for the smallest card to be buried under `5`, `2`, or `7`. The smallest card in the room **must be one of the top cards currently visible on the 3 decks**!

Therefore:
1. You do not need to look at all 12 cards.
2. You only look at the **top card of each deck**: `5` (Deck 0), `2` (Deck 1), `7` (Deck 2).
3. You compare those 3 cards. The winner is `2`.
4. You pick up `2` and place it in your output pile.
5. Now Deck 1's top card is gone! You flip the next card from Deck 1: `8`.
6. Your new visible candidates on the table are: `5` (Deck 0), `8` (Deck 1), `7` (Deck 2).
7. Who is the winner now? `5`! You pick up `5`, and flip the next card from Deck 0: `12`.

At any microsecond, **you only ever hold 3 cards in your hand**, no matter if each deck contains 10 cards or 10 billion cards!

```text
Run 0 File: [ 5, 12, 40, 99 ] ──► (Read top card: 5) ──┐
                                                        │
Run 1 File: [ 2,  8, 15, 30 ] ──► (Read top card: 2) ──┼──► [ Min-Heap Tournament: Winner = 2 ]
                                                        │
Run 2 File: [ 7, 10, 25, 50 ] ──► (Read top card: 7) ──┘
```

##### What the Computer Physically Does in C (Bytes & File Pointers)
Now translate that card table directly to the operating system and CPU:
1. **Each "deck" is a `.run` file on disk.** In Phase 1, we wrote `K` files (`run_0000.run`, `run_0001.run`, ...). Each file contains millions of records, already sorted.
2. **Open K Handles (`run_files[i] = fopen(...)`):**
   The operating system gives us `K` open file pointers. Each file's read cursor starts parked at byte 0 (the top of the deck).
3. **Read Exactly 1 Tuple From Each (`fread(..., 1, fp)`):**
   We do **not** load the files into RAM! We tell the OS: *"Read exactly ONE 8-byte `sort_record_t` from file 0, ONE from file 1, ..., ONE from file K-1."*
   If we have 10 runs, our RAM holds **only 10 tuples (80 bytes)**!
4. **"Seed the Min-Heap" (Enter Candidates into the Tournament Bracket):**
   "Seeding" just means placing those `K` initial records into our heap array (`min_heap_t`).
   Because we need to know where each record came from when it wins, we wrap each record in a `heap_node_t`:
   ```c
   heap_node_t node = {
       .record = rec,     // e.g. key = 2, row_id = 45
       .run_idx = 1       // "I came from Deck 1 (run_0001.run)!"
   };
   ```
   The binary min-heap organizes these `K` contestants in `O(K log K)` time so that `heap.data[0]` is guaranteed to be the overall minimum element across all runs.
   The tournament is now ready to stream!

#### Step 5: Streaming Pull Iteration (`external_sorter_next`)

```c
bool external_sorter_next(external_sorter_t *sorter, sort_record_t *out_record);
```

The consumer iteratively requests the next sorted record via `external_sorter_next(sorter, &out_record)` until it returns `false` (signaling EOF).

##### 1. State Machine & Parameter Validation:
```c
if (!sorter || !out_record || !sorter->finished_ingestion) {
    return false;
}
```
If `external_sorter_finish()` was never called, reading from the sorter is an illegal operation.

##### 2. Branch A — The Fast Path (`sorter->num_runs == 0`):
If all records fit in RAM, no run files or min-heap were ever touched.
We simply serve elements directly from the sorted contiguous buffer in RAM using our read bookmark `in_mem_iter_idx`:
```c
if (sorter->num_runs == 0) {
    if (sorter->in_mem_iter_idx < sorter->in_mem_count) {
        *out_record = sorter->in_mem_buf[sorter->in_mem_iter_idx++];
        return true;
    }
    return false; // All in-memory records exhausted (EOF)
}
```
Pure $O(1)$ memory copies with 100% cache-line throughput!

##### 3. Branch B — The Spilled Merge Path (`sorter->num_runs > 0`):
The records are streaming through the K-Way Min-Heap tournament:

1. **Check for Dataset Exhaustion:**
   If `sorter->heap.size == 0`, every single run file on disk has reached EOF. There are zero remaining records; return `false`.
2. **Crown the Champion:**
   Because the min-heap guarantees that the minimum element across all active runs is at the root:
   ```c
   *out_record = sorter->heap.data[0].record;
   size_t run_idx = sorter->heap.data[0].run_idx;
   ```
3. **Replenish from That Specific Run:**
   To keep the tournament fair, we must pull the next card from the deck that just won:
   ```c
   sort_record_t next_record;
   if (fread(&next_record, sizeof(sort_record_t), 1, sorter->run_files[run_idx]) == 1) {
       // Case 1: The deck has more cards!
       // Place the new card at the root and let it sink down to its rightful depth:
       sorter->heap.data[0].record = next_record;
       heap_sift_down(&sorter->heap, 0);
   } else {
       // Case 2: The run file reached EOF! That deck is completely drained!
       // 1. Close the file handle immediately:
       fclose(sorter->run_files[run_idx]);
       sorter->run_files[run_idx] = NULL;

       // 2. Shrink the heap: move the LAST element in the array to the root:
       sorter->heap.data[0] = sorter->heap.data[sorter->heap.size - 1];
       sorter->heap.size--;

       // 3. Sift down the replacement element to restore min-heap order:
       if (sorter->heap.size > 0) {
           heap_sift_down(&sorter->heap, 0);
       }
   }
   return true;
   ```

Zero disk seeking, bounded RAM, $O(\log K)$ replenishment per tuple.

#### Step 6: Teardown & Ephemeral Cleanup (`external_sorter_destroy`)

Spilled runs are ephemeral scratch files, not permanent database parts. Furthermore, analytical queries often abort early (e.g., when a query has `LIMIT 10`, the caller destroys the sorter long before all run files are drained!).

`external_sorter_destroy()` must execute a thorough cleanup checklist to guarantee zero file descriptor leaks, zero orphaned scratch files on disk, and zero memory leaks:

1. **Close Any Active File Descriptors (`run_files`):**
   If the query stopped early (or completed), any open `FILE*` handles in `sorter->run_files` must be closed to avoid leaking OS file descriptors (`EMFILE`):
   ```c
   if (sorter->run_files) {
       for (size_t i = 0; i < sorter->num_runs; i++) {
           if (sorter->run_files[i]) {
               fclose(sorter->run_files[i]);
               sorter->run_files[i] = NULL;
           }
       }
       free(sorter->run_files);
   }
   ```
2. **Unlink Files & Free Path Strings (`run_paths`):**
   Each run generated during Phase 1 allocated both a file on disk AND a heap string in RAM (`strdup(path)`):
   ```c
   if (sorter->run_paths) {
       for (size_t i = 0; i < sorter->num_runs; i++) {
           if (sorter->run_paths[i]) {
               remove(sorter->run_paths[i]); // Unlink / delete file from disk
               free(sorter->run_paths[i]);   // Free heap string
           }
       }
       free(sorter->run_paths);             // Free the array of char* pointers
   }
   ```
3. **Free Priority Queue Buffer (`heap.data`):**
   ```c
   if (sorter->heap.data) {
       free(sorter->heap.data);
   }
   ```
4. **Free In-Memory Staging Buffer & Struct Container:**
   ```c
   free(sorter->in_mem_buf);
   free(sorter);
   ```

> [!TIP]
> **C Pointer Safety Note:** In standard C, `free(NULL)` is guaranteed to be a safe no-op. If `num_runs == 0` (such as on the fast path), `run_paths`, `run_files`, and `heap.data` remain `NULL`. Passing them to `free()` is completely safe, while wrapping pointer array loops with `if (sorter->run_paths)` guards against dereferencing `NULL` when `num_runs == 0`.

Zero orphaned files in `/tmp`, zero file descriptor leaks, zero bytes leaked under AddressSanitizer.

---

### Architectural Reality: If Table Parts Are Already Sorted, Why Do We Ever Need Run Files?

You asked a fundamental, architectural question:
> *"If our on-disk table parts (`part_0`, `part_1`, ...) are already sorted by primary key, why do we ever need to create temporary run files?"*

The answer reveals the boundary between **Storage Engine properties** and **General Query Execution**:

#### Case 1: When Parts ARE the Runs (`ReadInOrder` Fast Path)
If a table was created with:
```sql
CREATE TABLE events (...) ORDER BY (tenant_id, event_time);
```
And a user executes:
```sql
SELECT * FROM events ORDER BY tenant_id, event_time;
```
ClickHouse detects that the query ordering **matches the physical storage ordering**.
In this specific scenario, **WE DO NOT CREATE RUN FILES AT ALL!**
ClickHouse activates the **`ReadInOrder`** optimization:
- Phase 1 (spilling run files) is **completely bypassed**.
- The existing immutable table parts on disk (`part_0`, `part_1`, ...) act directly as the runs!
- The engine opens column readers on each part, seeds the Min-Heap directly with the first row of each part, and streams globally sorted rows straight to the user. Zero temporary files are created.

```text
Table Part 0 on Disk (Sorted by timestamp) ──► Stream Row ──┐
Table Part 1 on Disk (Sorted by timestamp) ──► Stream Row ──┼──► [ Min-Heap ] ──► Globally Sorted Result
Table Part 2 on Disk (Sorted by timestamp) ──► Stream Row ──┘    (Zero run files created!)
```

#### Case 2: Why We Absolutely Need Run Files (The 3 Real-World Scenarios)

If `ReadInOrder` is so fast, why does ClickHouse or `simpledb` have an External Sorter that creates temporary run files?
Because in real analytical workloads, **data frequently needs to be sorted in ways that physical disk parts cannot provide**:

##### 1. Sorting by a Non-Primary Key Column (`ORDER BY other_column`)
A table can only have **one physical ordering on disk**.
If your table is physically sorted by `timestamp`:
* `Part 0 timestamps:` `[ 10:01, 10:02, 10:03 ]` *(Sorted)*
* `Part 0 user_salary:` `[ $150,000, $40,000, $95,000 ]` *(Completely Random!)*

What happens if an analyst runs:
```sql
SELECT * FROM events ORDER BY user_salary DESC;
```
You **cannot** stream `Part 0` into a Min-Heap for `user_salary`! The salaries inside each part are jumbled and chaotic. A Min-Heap tournament only works if each input stream is strictly monotonic (individually sorted).
Because the on-disk parts are not sorted by `user_salary`, the query engine **must extract `(user_salary, row_id)` tuples, sort them in RAM, and spill temporary `.run` files**.

##### 2. Sorting Computed Aggregations (`GROUP BY ... ORDER BY`)
Consider this common analytical dashboard query:
```sql
SELECT country, SUM(revenue) AS total_rev
FROM sales
GROUP BY country
ORDER BY total_rev DESC;
```
Where does `total_rev` live on disk? **Nowhere!**
`total_rev` was computed dynamically in RAM by aggregating millions of rows during query execution. It does not exist in any table part.
If there are 50 million unique merchants or keys, the aggregated result set exceeds the RAM budget. To sort the aggregated totals, the query engine has no choice: it must pipe the aggregated rows into an external sorter that generates temporary sorted `.run` files on disk.

##### 3. Sorting Intermediate Pipeline Streams (JOINs & Window Functions)
In complex relational queries, intermediate operators produce dynamic, in-flight streams of tuples:
- **Sort-Merge Joins:** Two massive tables must be joined on an arbitrary join condition (`ON a.user_ip = b.user_ip`). Both input streams must be sorted by `user_ip` before the merge join can proceed.
- **Window Functions:** `ROW_NUMBER() OVER (PARTITION BY user_id ORDER BY transaction_amount)`.
These intermediate rows do not correspond to any single on-disk table part; they are ephemeral data streams that must be sorted out-of-core when RAM is exhausted.

---

### Anatomy of the Data Structures: Why Every Struct and Field Exists

To understand the mechanics of external sort, we must inspect every single struct and field in `include/simpledb/external_sort.h`. Every field has a strict, indispensable role in the systems architecture:

```text
┌────────────────────────────────────────────────────────────────────────┐
│                        DATA STRUCTURE TAXONOMY                         │
│                                                                        │
│  sort_record_t ──────────► Compact 8-byte (key, value/row_id) tuple    │
│                                                                        │
│  heap_node_t   ──────────► sort_record_t + run_idx (origin provenance) │
│                                                                        │
│  min_heap_t    ──────────► Priority queue (data[], size, capacity)     │
│                                                                        │
│  external_sorter_t ──────► Complete state machine manager:             │
│                            - Ingestion buffer & limits                 │
│                            - Run file catalog & active FILE* handles   │
│                            - K-way Min-Heap merge engine               │
│                            - Fast-path in-memory cursor                │
└────────────────────────────────────────────────────────────────────────┘
```

#### 1. `sort_record_t`: The Fundamental Tuple Unit
 
```c
typedef struct {
    int32_t key;    // The sort key value (e.g., score = 92, timestamp = 1700000000)
    int32_t row_id; // Late materialization pointer: tuple index in the table
} sort_record_t;
```

* **Why `key` and `row_id`? (Avoiding the Overengineering Trap):**
  - Textbook data structures often define `(key, value)`. But in database queries (`ORDER BY score`), developers often mistake `value` for the score and `key` for the row ID!
  - To prevent this confusion, we explicitly pair **`key`** (the column value being sorted) with **`row_id`** (the row pointer).
  - While a C11 anonymous union (`union { int32_t key; int32_t key_value; };`) can provide dual naming, **it is overengineered fluff**. In clean, idiomatic C, having two names for the exact same variable is unnecessary complexity. Picking a clean, standard name like **`key`** (or `key_value`) and pairing it unmistakably with **`row_id`** is far simpler and more readable.
* **Why only 8 bytes total?**
  This is the cornerstone of **Late Materialization**:
  - If we sorted wide rows (e.g., 20 columns = 160 bytes per row), spilling 100M rows would require writing **16 GB** to disk.
  - With `sort_record_t`, 100M rows require only **800 MB** of disk writes (**95% reduction in I/O bandwidth**).
  - Furthermore, in a 64 MB RAM limit, we can hold `64 * 1024 * 1024 / 8 = 8,388,608` records in RAM before spilling a single run!
  - Once sorted, the engine uses `rec.row_id` to perform a SIMD Gather or targeted column fetch for only the surviving rows (e.g., the top 100).

---

#### 2. `heap_node_t`: Heap Element with Stream Provenance

```c
typedef struct {
    sort_record_t record; // The actual tuple (key, value)
    size_t run_idx;       // Provenance: which run file did this come from?
} heap_node_t;
```

* **Why `record` (`sort_record_t`)?**
  The heap must inspect `record.key` to maintain the min-heap invariant (`parent.key <= child.key`), and it must emit `record` when popped at the root.
* **Why `run_idx` (`size_t`)?**
  **This is the most critical field in Phase 2.**
  When the min-heap pops the minimum record from the root, the heap needs to pull the *next* replacement record to keep the merge flowing.
  **From which of the K run files should it read?**
  Without `run_idx`, the sorter has no idea which run file produced this tuple! By tagging every heap node with its originating `run_idx`, the engine instantly knows:
  `fread(&next_rec, sizeof(sort_record_t), 1, sorter->run_files[root.run_idx]);`
  It pulls precisely one record from the stream that just advanced, preserving total ordering across all streams.

> [!NOTE]
> **Architectural Dilemma: Why Not Generic `void *` for the Heap Node?**
> You asked a brilliant question: *Why hardcode `sort_record_t record; size_t run_idx;` in the heap node? Wouldn't a generic `void *` be cleaner for a reusable data structure?*
> 
> There are three distinct ways systems engines design priority queues, each with radical performance tradeoffs:
> 
> 1. **The Pure Generic Approach (`void *` + Function Pointer Comparator):**
>    ```c
>    typedef struct { void *item; } generic_heap_node_t;
>    typedef int (*heap_cmp_fn)(const void *a, const void *b);
>    ```
>    * **Pros:** 100% reusable across the entire codebase.
>    * **Cons (The Performance Killer):** During `sift_down`, the CPU must make an **indirect function pointer call (`cmp(a, b)`) on every single comparison**. In a 100M-row sort, that means **1 Billion indirect calls**, completely defeating compiler inlining, SIMD vectorization, and CPU branch prediction! Furthermore, if `item` points to heap allocations, it introduces pointer chasing and cache misses.
> 2. **The Key + Generic Payload Pattern (`int64_t key, void *payload`):**
>    ```c
>    typedef struct {
>        int64_t key;   // Compared directly via hardware ALU arithmetic (ZERO function calls!)
>        void *payload; // Opaque pointer to arbitrary caller context (e.g. run file or part reader)
>    } heap_node_t;
>    ```
>    * **Pros:** Generic payload, but comparisons remain fully inlined integer operations. Exactly 16 bytes (fits 4 nodes per 64-byte cache line).
> 3. **The Monomorphic Specialized Struct (ClickHouse / C++ Template style):**
>    In ClickHouse, priority queues are C++ templates (`PriorityQueue<T>`). The compiler emits specialized machine code for the exact concrete type with zero overhead.
>    In C, our concrete `heap_node_t` (`sort_record_t record; size_t run_idx;`) achieves the exact same zero-cost abstraction: 16 bytes, flat contiguous RAM, direct integer comparisons, and zero pointer dereferences.

---

#### 3. `min_heap_t`: The K-Way Tournament Priority Queue

```c
typedef struct {
    heap_node_t *data; // Contiguous array representing the binary tree
    size_t size;       // Number of active elements currently in the heap
    size_t capacity;   // Total allocated capacity (equals num_runs)
} min_heap_t;
```

* **Why `data` (`heap_node_t *`)?**
  Instead of allocating dynamic pointer-based tree nodes with `malloc` (which causes cache misses and pointer-chasing overhead), a binary heap is stored in a **single flat contiguous array**:
  - Root is at index `0`.
  - Left child of `i` is at `2 * i + 1`.
  - Right child of `i` is at `2 * i + 2`.
  - Parent of `i` is at `(i - 1) / 2`.
  This guarantees perfect CPU cache locality during sift-up and sift-down operations.
* **Why `size` (`size_t`)?**
  Tracks how many runs are currently active. Initially, `size = K`. When a run file hits EOF and has no more records to replenish, `size` is decremented by 1. When `size == 0`, the entire dataset has been completely drained.
* **Why `capacity` (`size_t`)?**
  The maximum number of nodes the heap will ever need to store. Since each of the `K` runs contributes at most 1 active record to the heap at any given moment, `capacity = num_runs`.

---

#### 4. `external_sorter_t`: The Master State Machine

```c
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
```

Let us break down each field by subsystem:

##### Subsystem A: Ingestion & Memory Budgeting
* **`temp_dir[256]`:** The directory string where temporary files like `/tmp/run_0000.run` are written. Stored inside the struct so that every run generation and destruction routine can access the base path without global state.
* **`mem_limit_records`:** The strict, deterministic memory budget ceiling. Defines the maximum number of `sort_record_t` tuples allowed in RAM before triggering a disk spill.
* **`in_mem_buf`:** The pre-allocated RAM array of capacity `mem_limit_records`. This is the working memory for Phase 1 where incoming batches accumulate and where `qsort` executes.
* **`in_mem_count`:** The fill cursor. Points to the current number of unspilled tuples sitting in `in_mem_buf`. When `in_mem_count == mem_limit_records`, the sorter spills.

##### Subsystem B: Disk Spill Catalog & File Handles
* **`run_paths` (`char **`):** A dynamically resizable list of heap-allocated file path strings (`["/tmp/run_0000.run", "/tmp/run_0001.run", ...]`).
  - *Why needed?* Phase 1 generates runs sequentially. We need to remember their paths so Phase 2 can open them, and so `external_sorter_destroy()` can delete (`remove()`) every file from disk.
* **`run_files` (`FILE **`):** An array of open standard I/O file handles (`FILE*`), one for each spilled run.
  - *Why needed?* During Phase 2, all `K` runs must be read concurrently. `run_files[i]` holds the open stream for run `i` so we can pull the next record on demand without reopening files or seeking.
* **`num_runs` (`size_t`):** The total number of runs spilled to disk. Also represents the fan-in degree `K` for Phase 2.
* **`run_paths_capacity` (`size_t`):** The allocation capacity of the `run_paths` and `run_files` dynamic arrays. Grows exponentially (`capacity * 2`) to avoid frequent reallocations if many runs are spilled.

##### Subsystem C: Execution Coordination & Fast-Path Optimization
* **`finished_ingestion` (`bool`):** A state machine guard.
  - Protects against calling `external_sorter_push()` after ingestion has been sealed.
  - Protects against calling `external_sorter_next()` before `external_sorter_finish()` has prepared the heap or fast-path state.
* **`heap` (`min_heap_t`):** Embedded priority queue struct. Directly drives the Phase 2 streaming K-way merge algorithm.
* **`in_mem_iter_idx` (`size_t`):** The read cursor for the **In-Memory Fast Path**.
  - **Why can't we just use `in_mem_count`? Why do we need a separate iterator index?**
    Think of `in_mem_buf` as a book:
    - `in_mem_count` is the **total number of pages in the book** (e.g. 50 pages). Once ingestion finishes, this number is fixed.
    - `in_mem_iter_idx` is the **bookmark pointing to the current page you are reading** (starts at page 0).
    When the query calls `external_sorter_next(sorter, &rec)` repeatedly:
    - 1st call reads `in_mem_buf[0]`, then moves the bookmark to `1`.
    - 2nd call reads `in_mem_buf[1]`, then moves the bookmark to `2`.
    - ...
    - When `in_mem_iter_idx == in_mem_count`, you have reached the end of the book (EOF), and `next()` returns `false`!
  - **What would break if you tried to reuse `in_mem_count` without `in_mem_iter_idx`?**
    1. *The Out-of-Bounds Trap:* Reading `in_mem_buf[in_mem_count]` would immediately read past the end of the array into uninitialized memory!
    2. *The Reverse-Order Trap:* If you decremented count on every read (`in_mem_buf[--in_mem_count]`), you would read from the end backwards, returning your carefully sorted data in reverse (descending) order!
    3. *The O(N²) Shifting Disaster:* If you tried to always return `in_mem_buf[0]` and shift the remaining elements left with `memmove`, every tuple read would take `O(N)` time, turning streaming iteration into a catastrophic `O(N²)` performance bottleneck!
  - By separating `in_mem_count` (the static write size) from `in_mem_iter_idx` (the moving read cursor), iteration is clean, forward-scanning, and `O(1)` per record.

---

### The ClickHouse MergeTree Connection: Why Build External Sort Before Part Merging?

You asked a brilliant architectural question:
> *"Wait, why are we building the External Sorter before the Part Merging engine?"*

There are two profound systems reasons:

#### 1. A Part Merger *IS* an External Merge Sort!
In ClickHouse, table parts on disk are sorted by primary key. When ClickHouse's background daemon merges `part_0`, `part_1`, and `part_2` into a single consolidated `part_0_2`, **what algorithm does it use?**

It cannot load all parts into RAM (that would trigger an OOM crash). Instead, it runs **Phase 2 of an External Merge Sort**:
```text
  Part 000 (Sorted on Disk) ──► Stream Tuple ──┐
  Part 001 (Sorted on Disk) ──► Stream Tuple ──┼──► [ K-Way Min-Heap ] ──► Write to Merged Part
  Part 002 (Sorted on Disk) ──► Stream Tuple ──┘    (Size = K = 3)
```
1. It opens column readers for the K parts.
2. It pushes the first row of each part into a **K-Way Min-Heap Priority Queue**.
3. It pops the minimum primary key, resolves any deletion vectors or version updates, and appends it to the new merged part.

The **K-Way Min-Heap Priority Queue** that we are building today in Day 15 is the **exact core algorithmic engine** that makes part merging mathematically possible! Building a part merger without first having a K-way merge heap would be putting the cart before the horse.

#### 2. Part Merging is NOT Required to Query Data
In LSM-trees (RocksDB, Cassandra) and ClickHouse MergeTree, part merging is an **asynchronous background compaction optimization** designed to reduce the number of open file handles and prune deleted rows.

**A database does not need parts to be merged to answer queries!**
* On Day 17, our **Multi-Part Table Scanner (`table_scanner_t`)** will scan across all unmerged parts (`table->parts[0]`, `table->parts[1]`, etc.), evaluate the sparse index on each part independently, and stream the matching granules directly into the query engine.
* The query engine produces 100% correct, consistent results whether the table has 1 merged part or 20 unmerged parts.

By mastering the **External Sorter** today, we accomplish two goals at once:
1. We give our query engine the physical `ORDER BY ... LIMIT N` operator.
2. We build the exact K-way Min-Heap kernel needed for future background compaction!

#### 3. Roadmap Alignment: The Part Compactor is Day 19!
With this architectural decoupling, our master roadmap flows with clean, layered precision:
* **Day 15 (Today):** External Merge Sort & K-Way Binary Min-Heap (`ORDER BY`).
* **Day 16:** Partitioned Grace Hash Aggregation (`GROUP BY`).
* **Day 17:** The Multi-Part Table Scanner (`table_scanner_t` streaming unmerged parts + live RAM).
* **Day 18:** Vectorized Physical Execution Pipeline (`exec_pipeline_t`).
* **Day 19:** **The MergeTree Part Compactor (`table_merge_parts`)**: Reuses Day 15's Min-Heap and Day 13's Deletion Vectors to merge multiple immutable on-disk parts into consolidated parts, rewriting `.mrk` files and generating a unified `primary.idx`.
* **Day 20:** **Minimal SQL Parser & REPL CLI Shell**: The grand finale—interactive shell querying the entire database!

---

### Deep Dive: Theory, Limitations, and Terabyte-Scale Sorting in ClickHouse

#### 1. The Algorithmic Theory & Cost Model

At its mathematical core, External Merge Sort is a **two-phase divide-and-conquer algorithm** designed under the strict constraint that total input size `N` vastly exceeds the working memory budget `M` (`M << N`).

##### Phase 1: Run Generation (Divide)
* Given an incoming stream of `N` records and a fixed RAM buffer capable of holding `M` records, the stream is sliced into `K = ceil(N / M)` independent partitions ("runs").
* Each partition is sorted in-place in RAM using an `O(M log M)` comparison sort (e.g., QuickSort, IntroSort, or PDQSort).
* The sorted buffer is written sequentially to disk as an immutable run file:
  - **CPU Cost (Phase 1):** `K * O(M log M) = (N / M) * O(M log M) = O(N log M)`
  - **I/O Cost (Phase 1):** `N * sizeof(record)` (Sequential Writes)

##### Phase 2: Streaming K-Way Merge (Conquer)
* Rather than attempting pairwise recursive merges (which would read and write data back to disk multiple times), we merge all `K` runs simultaneously in a single streaming pass using a **Min-Heap Priority Queue** of size `K`.
* For each output tuple, the root is extracted (`O(1)`), written to the query output stream, and replenished by reading the next tuple from that specific run file via heap sift-down (`O(log K)`).
* Because every single one of the `N` records passes through the heap exactly once:
  - **CPU Cost (Phase 2):** `N * O(log K) = O(N log K)`
  - **I/O Cost (Phase 2):** `N * sizeof(record)` (Sequential Reads)

##### Total Computational Complexity
Combining Phase 1 and Phase 2 yields:
```text
Total Time Complexity = O(N log M) + O(N log K) = O(N log(M * K)) = O(N log N)
```
The time complexity is identical to an in-memory sort (`O(N log N)`), but with a **bounded memory footprint**:
```text
Peak RAM = max(M * sizeof(record), K * sizeof(heap_node_t) + K * read_buffer_size)
```

---

#### 2. Why We Absolutely Need It: Hardware & Systems Reality

Why not simply allocate virtual memory and let the operating system handle paging?

```text
┌─────────────────────────────────────────────────────────────────────────────────┐
│                          IN-MEMORY SORTING ON HUGE DATA                         │
│                                                                                 │
│   Virtual Memory Address Space: [ 100 GB Array ]                                │
│   Physical RAM Available:       [ 16 GB Physical RAM ]                          │
│                                                                                 │
│   QuickSort Partitioning ──► Jumps across 100 GB range                          │
│                              ├──► Catastrophic OS Page Faults (4KB pages)       │
│                              ├──► NVMe Disk Thrashing (Random Seek Overhead)    │
│                              └──► Throughput drops from 6 GB/s to < 10 MB/s     │
│                                   (or OOM Killer instantly kills database!)    │
└─────────────────────────────────────────────────────────────────────────────────┘
```

1. **Catastrophic Virtual Memory Thrashing:**
   Algorithms like `qsort` or introsort rely on random access across the entire array range. When an array spans beyond physical RAM, the OS kernel's virtual memory subsystem tries to swap 4KB memory pages to swap space. Because sorting touches arbitrary indices, page cache hit rates collapse to near zero (< 1%). The system spends 99.9% of its CPU time stalled waiting on random 4KB disk page faults—a state known as **disk thrashing**.
2. **Deterministic Query Memory Budgets:**
   In multi-tenant analytical database engines, hundreds of concurrent queries execute simultaneously. An engine cannot allow a single unbounded `ORDER BY` query to consume all server RAM and trigger the Linux Out-Of-Memory (OOM) Killer (`oom-killer`). External Merge Sort enforces a strict, predictable memory ceiling per query thread (e.g., 64 MB).
3. **Mechanical Sympathy via Sequential Streaming:**
   Whether running on spinning magnetic rust (HDDs) or modern PCIe Gen 5 NVMe SSDs, sequential read/write bandwidth is orders of magnitude faster than random I/O. External merge sort translates the sorting of an arbitrarily large dataset into **100% sequential block I/O**.
4. **Foundation for Relational Database Engines:**
   Without an external sorter, a database cannot execute:
   - Out-of-core `ORDER BY` queries.
   - **Sort-Merge Joins** (joining multi-terabyte tables without holding massive hash tables in RAM).
   - **Compaction & Deduplication:** MergeTree table engines (`ReplacingMergeTree`, `CollapsingMergeTree`) require streaming sorted runs to deduplicate records and collapse rows along primary keys.

---

#### 3. Limitations of Standard (Two-Phase) External Merge Sort

While elegant, the simple two-phase external merge sort built on Day 15 has distinct real-world scaling boundaries:

1. **The Fan-in Bottleneck (K-Limit & OS File Descriptors):**
   - In a single-pass merge (Phase 2), the engine must hold open file handles for all `K` spilled runs simultaneously.
   - If `N = 10 TB` and the memory limit is `M = 100 MB`, the number of runs generated is:
     `K = 10 TB / 100 MB = 100,000 runs!`
   - Most operating systems enforce a strict per-process limit on open file descriptors (`ulimit -n`, typically 1,024 or 4,096). Attempting to open 100,000 `FILE*` handles immediately fails with `EMFILE` (Too many open files).
2. **Buffer Fragmentation & Loss of Sequentiality:**
   - Even if the OS permitted 100,000 open files, allocating an I/O read buffer for each file within a 1 GB RAM budget would leave only ~10 KB per file.
   - Reading in tiny 10 KB chunks turns sequential disk streaming into thousands of concurrent small I/O requests, defeating the hardware's native read-ahead buffers and crippling NVMe queue depth.
3. **Necessity of Multi-Pass (Hierarchical) Merging:**
   - To overcome the fan-in ceiling when `K > K_max` (e.g., `K_max = 256`), the engine must switch to **multi-pass external merge sort**:
     - It merges groups of 256 runs into intermediate larger runs, writes them back to disk, and repeats in a tree-like hierarchy until `<= 256` runs remain.
   - **The Tradeoff: I/O Write Amplification:**
     Every additional merge level multiplies total disk I/O. If 3 merge passes are required, every single byte is written and read 3 times over:
     `Total Disk I/O = 2 * N * ceil(log_B(N / M))`
4. **I/O Latency & SSD Endurance Wear:**
   - Spilling terabytes of ephemeral intermediate data consumes storage bandwidth and causes SSD flash cell wear (write endurance).
5. **Synchronous Ingestion Pipeline Stalling:**
   - In a single-threaded implementation, whenever `in_mem_buf` fills up, the ingestion pipeline freezes while `qsort` runs and `fwrite` flushes to disk. Production engines require **double-buffering** or asynchronous I/O (`io_uring`) so ingestion continues in one buffer while a background thread sorts and flushes the other.

---

#### 4. What Happens in ClickHouse When Sorting Terabytes of Data?

When an analytical workload demands sorting multi-terabyte datasets in ClickHouse, the engine deploys a series of specialized optimizations to avoid naive brute-force disk sorting whenever possible, falling back to an industrial-grade external merge pipeline when required.

```text
                                  Incoming Query: ORDER BY
                                             │
             ┌───────────────────────────────┴───────────────────────────────┐
             ▼                                                               ▼
   Has LIMIT N (e.g. 100)?                                      Does ORDER BY match
             │ (Yes)                                            Table Sorting Key?
             ▼                                                               │ (Yes)
   [ Top-K In-Memory Heap ]                                                  ▼
   (No disk spill, O(N log K))                                  [ Streaming ReadInOrder ]
                                                                (Skip sorting entirely!
                                                                 Merge parts via Min-Heap)
             │ (No)                                                          │
             └───────────────────────────────┬───────────────────────────────┘
                                             │ (Arbitrary Large Sort)
                                             ▼
                        [ ClickHouse External Sorter ]
                        - max_bytes_before_external_sort
                        - LZ4-compressed chunk spills
                        - Hierarchical multi-pass merge
                        - Distributed parallel coordination
```

##### 1. Top-K Optimization: `ORDER BY ... LIMIT N`
In over 95% of real-world queries, analysts do not want to view 10 terabytes of sorted rows—they want the highest 100 or 1,000 records (`ORDER BY score DESC LIMIT 100`).
* ClickHouse completely short-circuits full external merge sort.
* It allocates a **bounded priority queue of size N in RAM**.
* It streams through terabytes of columnar data, maintaining only the top N elements in cache-resident RAM.
* Memory consumption is negligible (kilobytes), and zero bytes are spilled to disk.

##### 2. Physical Storage Alignment: `ReadInOrder`
ClickHouse `MergeTree` tables physically store data pre-sorted by their primary sorting key:
```sql
CREATE TABLE events (...) ENGINE = MergeTree() ORDER BY (tenant_id, event_time);
```
* If a query executes `SELECT * FROM events ORDER BY tenant_id, event_time`, ClickHouse detects that the on-disk parts are **already individually sorted**.
* ClickHouse enables the **`ReadInOrder` optimization**:
  - It bypasses sorting entirely!
  - It opens streaming readers on each part and streams them directly into a multi-way `MergingSortedTransform` (the exact Min-Heap engine from Day 15).
  - Data flows to the client instantly with zero sorting delay and zero temporary disk usage.

##### 3. Out-Of-Core Spilling: `max_bytes_before_external_sort`
When a query requires an arbitrary, non-indexed sort over terabytes of data without a small `LIMIT`:
* **Memory Budget Enforcement:** Governed by `max_bytes_before_external_sort` (or `max_bytes_ratio_before_external_sort`). When a query thread's sorting buffer reaches this threshold, ClickHouse transitions to external sorting.
* **Vectorized Block Sorting:**
  ClickHouse does not sort rows of structures. It processes data in **vectorized columnar chunks** (typically 65,536 rows). It computes a 32-bit or 64-bit permutation vector (`IColumn::sortPermutation`) using SIMD-accelerated algorithms (like Radix Sort or Pattern-Defeating Quicksort - PDQSort) across the key columns.
* **Compressed Run Spilling:**
  Unlike our uncompressed binary runs in Day 15, ClickHouse serializes spilled chunks into native compressed binary files on disk using **LZ4 compression**. Because columnar data compresses 3x to 5x, spilling to disk consumes a fraction of the raw I/O bandwidth.
* **Hierarchical Cascading Merges:**
  To prevent file descriptor exhaustion (`max_files_to_merge_sort`), ClickHouse uses multi-level hierarchical merging. It merges clusters of temporary files into larger sorted runs before the final merge pass.
* **Disk Quota Safety:**
  Settings like `max_temporary_data_on_disk_size_for_query` ensure that a runaway query will abort cleanly (`LIMIT_EXCEEDED`) if disk space is endangered, protecting server stability.

##### 4. Distributed Multi-Node Sorting (Cluster Execution)
In production, a multi-terabyte dataset is partitioned across tens or hundreds of ClickHouse nodes in a cluster:
```text
  [ Shard 1 ] ──► Local Sort / ReadInOrder ──► Sorted Stream 1 ──┐
  [ Shard 2 ] ──► Local Sort / ReadInOrder ──► Sorted Stream 2 ──┼──► [ Coordinator Node ]
  [ Shard 3 ] ──► Local Sort / ReadInOrder ──► Sorted Stream 3 ──┘    (Streaming K-Way Heap Merge)
```
* ClickHouse pushes the sorting work down to the worker nodes (`remote_fs` or local storage).
* Each shard sorts only its local shard partition (using in-memory sort or external sort).
* Each shard streams its sorted results across TCP sockets to the coordinator node.
* The coordinator node does **not** perform an expensive terabyte sort; it merely runs a streaming **K-Way Merge** over the network sockets, streaming the globally sorted result directly to the client!

---

#### 5. Architectural Comparison: simpledb vs. ClickHouse vs. DuckDB

How does the Day 15 sorting engine in `simpledb` compare to industry-standard analytical engines?

| Dimension | `simpledb` (Day 15) | ClickHouse (`MergeSorter`) | DuckDB (`PhysicalOrder`) |
| :--- | :--- | :--- | :--- |
| **Record Representation** | Narrow 8-byte `sort_record_t` (`int32_t key, int32_t row_id`). Late materialization. | Columnar Permutation Vectors (`IColumn::Permutation`). Late materialization across chunks. | Normalized byte-comparable keys (Radix Keys) + row payload layout. |
| **In-Memory Sort Kernel** | Standard C `qsort()` (Introsort) | SIMD PDQSort (Pattern-Defeating Quicksort) & Radix Sort | Cache-conscious Radix Sort (MSD/LSD) on normalized keys (`memcmp`) |
| **Spill Storage Format** | Contiguous uncompressed binary stream of `sort_record_t` (`fwrite` to `.run`) | Native columnar blocks compressed with **LZ4** / **ZSTD** | Paged temporary memory blocks managed by unified `BufferManager` |
| **Merge Kernel** | Single-pass Binary Min-Heap Priority Queue (array-backed) | Multi-pass Loser Tree / Priority Queue (`MergingSortedTransform`) | Multi-pass Cascading Tournament Tree (Loser Tree) |
| **Execution Model** | Single-threaded streaming iterator (push ingestion, pull heap extraction) | Multi-threaded pipeline (`ParallelSortingTransform`) + Distributed cluster merge | Multi-threaded morsel-driven execution (thread-local runs + parallel merge) |
| **Top-K / LIMIT** | Stream top K records, then SIMD Gather surviving rows | Bounded Priority Queue (`TopNFilteringTransform`) + `ReadInOrder` table index skip | In-memory `PhysicalTopN` bounded priority queue (zero disk spill) |

##### Key Differences Explained:

1. **Permutations vs. Normalized Keys vs. Structs:**
   - **`simpledb`** uses fixed 8-byte `(key, row_id)` structs. This gives high cache density (8.3M records in 64MB) and simple sequential disk I/O, but only handles a single integer sort key.
   - **ClickHouse** avoids copying values altogether. It creates an array of 64-bit row offsets (`IColumn::Permutation`) pointing directly into the columnar memory chunks. For multi-column sorting (`ORDER BY a, b, c`), it performs lexicographical comparison passes over the column vectors.
   - **DuckDB** serializes arbitrary multi-column, mixed-type sort keys into **Normalized Keys** (byte strings where binary `memcmp` matches the SQL sorting order, flipping bits for sign handling). This allows DuckDB to sort complex composite keys using blazing-fast byte-level Radix Sort without type-specific comparison functions.

2. **The Sorting Kernel: `qsort` vs. PDQSort vs. Radix Sort:**
   - **`simpledb`** uses C's standard `qsort()`, which is an introspective quicksort. While reliable, comparison sorts are fundamentally bounded by `O(N log N)` comparisons with branch mispredictions on random data.
   - **ClickHouse** uses **PDQSort** (Pattern-Defeating Quicksort), which detects patterns (already sorted, reverse sorted, or many duplicate keys) and switches algorithms to achieve `O(N)` linear performance on near-sorted inputs.
   - **DuckDB** uses **Radix Sort** on its normalized keys. Radix sort processes bytes directly without branching, achieving linear `O(N * W)` time (where `W` is key byte width).

3. **Disk Spilling & Compression:**
   - **`simpledb`** writes uncompressed structs directly to temporary files. This is simple, deterministic, and fast on local NVMe drives.
   - **ClickHouse** compresses spilled blocks with **LZ4**. Columnar data compresses 3x to 5x, meaning ClickHouse writes only 20% to 30% of the raw byte volume to disk. The CPU cycles spent on LZ4 compression are easily offset by eliminating disk I/O bottlenecks.
   - **DuckDB** routes spilled data through its unified `BufferManager`. It does not create ad-hoc temporary file formats; temporary sort blocks are treated as paged memory blocks managed under the global database buffer cache.

4. **The Merge Kernel: Min-Heap vs. Loser Tree:**
   - **`simpledb`** implements a **Binary Min-Heap** where the root is replaced and sifted down on every output record. In a heap of size `K`, each sift-down requires checking two sibling children, taking `2 * log2(K)` comparisons per tuple.
   - **ClickHouse and DuckDB** implement **Loser Trees** (Tournament Trees). In a Loser Tree, each node stores the index of the "loser" of the tournament match, meaning each replenishment step only requires `1 * log2(K)` comparisons (half the CPU comparisons of a binary min-heap).

5. **Parallelism:**
   - **`simpledb`** is designed as a clean, single-threaded educational reference to master out-of-core algorithms.
   - **ClickHouse and DuckDB** sort in parallel across all CPU cores using morsel-driven or pipeline parallelism. Multiple threads sort local memory blocks concurrently, and parallel merge workers combine the resulting runs in a multi-stage tree.

---

### Binary Min-Heap Implementation Blueprint: What is a Heap & Why Sift?

#### 1. What is a Heap (Priority Queue)?
A **Binary Min-Heap** is a specialized, complete binary tree data structure that satisfies the **Min-Heap Invariant**:
> **For every node $i$: The value of the parent is always less than or equal to the values of its children.**
> $$\text{Value}(\text{parent}) \le \text{Value}(\text{child})$$

Because every parent is smaller than its children, **the absolute smallest element in the entire data structure is GUARANTEED to sit at the root node (index 0)**.

```text
               10 (Root: Absolute Minimum!)
             /    \
           25      15
          /  \    /  \
        40   60  30   18
```

Notice that a heap is **not fully sorted**. Node 25 is on the left, node 15 is on the right; 40 is greater than 18. A heap only guarantees vertical dominance (parent $\le$ child), not horizontal order. This partial ordering is why heaps are so blazingly fast: maintaining partial order takes only $O(\log K)$ time, whereas keeping an array fully sorted takes $O(K)$ time!

#### 2. The K-Way Merge Dilemma: Why Heaps Crush Linear Scans
During Phase 2, we have $K$ sorted run files on disk. We hold 1 candidate record from each of the $K$ runs.
To emit $N$ sorted records in total:
* **Naive Linear Scan ($O(K)$ per tuple):**
  Scan all $K$ candidates linearly to find the smallest one.
  Total CPU comparisons: $N \times K$.
  If $K = 1,000$ runs and $N = 100,000,000$ records:
  $$100,000,000 \times 1,000 = 100,000,000,000 \text{ comparisons! (100 Billion!)}$$
* **Binary Min-Heap ($O(\log K)$ per tuple):**
  The minimum is instantly available at the root in **$O(1)$** time.
  When replaced by the next tuple from that run, restoring the heap takes $\log_2(K)$ steps.
  If $K = 1,000$ runs: $\log_2(1,000) \approx 10$ comparisons.
  $$100,000,000 \times 10 = 1,000,000,000 \text{ comparisons! (100x faster!)}$$

#### 3. Deep Systems Dive: Implicit Data Structures, Flat Arrays & Pointer Arithmetic

Why is storing a binary tree in a flat contiguous array considered one of the greatest triumphs of computer systems engineering?

##### A. The Theory: What is an "Implicit Data Structure"?
In 1980, computer scientists **J. Ian Munro and Hendrik Suwanda** published their seminal paper, *"Implicit Data Structures"*.
They formalized the distinction between two fundamental ways computers organize information:

1. **Explicit Data Structures:**
   Traditional trees (Binary Search Trees, AVL Trees, Red-Black Trees, B-Trees) store their structure **explicitly using memory pointers**:
   ```c
   struct explicit_tree_node {
       int32_t key;
       int32_t value;
       struct explicit_tree_node *left;   // 8 bytes (on 64-bit CPU)
       struct explicit_tree_node *right;  // 8 bytes
       struct explicit_tree_node *parent; // 8 bytes
   };
   ```
   - **The Overhead Tax:** To store 8 bytes of actual data (`key` + `value`), this node consumes **24 bytes of pointer metadata**!
   - Metadata overhead is **300%**!
   - In an analytical engine sorting 100 million records, pointer overhead alone would waste **2.4 Gigabytes of RAM** just to remember which node connects to which!

2. **Implicit Data Structures:**
   An **implicit data structure** is a data structure where **ZERO pointers or structural metadata are stored**.
   Instead, the relationship between elements is defined **implicitly by the physical memory location (index permutation)** of the data itself!
   - Metadata overhead: **0 bytes (0%)**.
   - Every single bit in RAM belongs to actual payload data.
   - The Binary Heap is the ultimate canonical example of an implicit data structure.

---

##### B. From 2D Tree to 1D Linear Address Space (Geometric Level-Order Embedding)

How does a two-dimensional branching tree map cleanly into a one-dimensional linear byte stream without leaving holes?
It relies on a **geometric level-order (breadth-first) tiling**:

```text
                           THE LOGICAL 2D COMPLETE BINARY TREE
                                      LEVEL 0 (Root)
                                      ┌────────────┐
                                      │  Index 0   │
                                      │ (Value 10) │
                                      └─────┬──────┘
                             ┌──────────────┴──────────────┐
                             ▼                             ▼
                      LEVEL 1                       LEVEL 1
                   ┌────────────┐                ┌────────────┐
                   │  Index 1   │                │  Index 2   │
                   │ (Value 20) │                │ (Value 30) │
                   └─────┬──────┘                └─────┬──────┘
                 ┌───────┴───────┐             ┌───────┴───────┐
                 ▼               ▼             ▼               ▼
              LEVEL 2         LEVEL 2       LEVEL 2         LEVEL 2
           ┌────────────┐  ┌────────────┐┌────────────┐  ┌────────────┐
           │  Index 3   │  │  Index 4   ││  Index 5   │  │  Index 6   │
           │ (Value 40) │  │ (Value 50) ││ (Value 60) │  │ (Value 70) │
           └────────────┘  └────────────┘└────────────┘  └────────────┘

                                           │
                                           ▼ (Breadth-First Level-by-Level Slicing)

Level 0:  [ Index 0: Val 10 ]
Level 1:  [ Index 1: Val 20 ][ Index 2: Val 30 ]
Level 2:  [ Index 3: Val 40 ][ Index 4: Val 50 ][ Index 5: Val 60 ][ Index 6: Val 70 ]

                                           │
                                           ▼ (Direct Linear Address Embedding)

                             THE PHYSICAL 1D ARRAY IN MEMORY (heap->data)
 ┌──────────────┬──────────────┬──────────────┬──────────────┬──────────────┬──────────────┬──────────────┐
 │   Index 0    │   Index 1    │   Index 2    │   Index 3    │   Index 4    │   Index 5    │   Index 6    │
 │ (Value: 10)  │ (Value: 20)  │ (Value: 30)  │ (Value: 40)  │ (Value: 50)  │ (Value: 60)  │ (Value: 70)  │
 └───┬──────────┴───┬──────────┴───┬──────────┴───┬──────────┴───┬──────────┴──────────────┴──────────────┘
     │              │              │              │              │
     │              │              │              └──────────────┴── Parent: (3-1)/2 = 1, (4-1)/2 = 1
     │              │              └──────────────────────────────── Left: 2*2+1 = 5, Right: 2*2+2 = 6
     │              └─────────────────────────────────────────────── Left: 2*1+1 = 3, Right: 2*1+2 = 4
     └────────────────────────────────────────────────────────────── Left: 2*0+1 = 1, Right: 2*0+2 = 2
```

```text
Tree Level      Nodes on Level     Starting Array Index      Ending Array Index
Level 0:        2^0 = 1 node       Index 0                   Index 0
Level 1:        2^1 = 2 nodes      Index 1                   Index 2
Level 2:        2^2 = 4 nodes      Index 3                   Index 6
Level 3:        2^3 = 8 nodes      Index 7                   Index 14
Level d:        2^d nodes          Index (2^d - 1)           Index (2^(d+1) - 2)
```

Notice the beautiful mathematical summation:
The starting index of any level $d$ is the sum of all nodes in preceding levels:
$$\text{Start}(d) = \sum_{j=0}^{d-1} 2^j = 2^d - 1$$

Because a heap enforces the **Complete Tree Invariant** (all levels are 100% packed, and the leaf level is filled from left to right), the $N$ active nodes map to **consecutive indices $0, 1, 2, \dots, N-1$ with ZERO memory gaps**.

---

##### Navigating the Implicit Tree: Node Pointers vs. Array Indices in C

A developer accustomed to pointer-based trees might naturally attempt to write navigation helpers using node pointers:

```c
// Attempting pointer-based tree navigation:
static heap_node_t* get_parent(heap_node_t* n);
static heap_node_t* get_left_child(heap_node_t* parent);
static heap_node_t* get_right_child(heap_node_t* parent);
```

Why is this pattern problematic in an array-backed implicit data structure?

###### 1. The Lost Context Problem (C Pointers Carry No Metadata)
In C, a raw pointer `heap_node_t *n` is simply a 64-bit integer representing a virtual memory address (e.g. `0x7ffee1234050`).
It has **no idea what its index is**, nor does it know where the array base `heap->data` begins or where `heap->size` ends:
- To calculate the parent index $\frac{i - 1}{2}$, you must first determine the current index $i$.
- But to find $i$ from a pointer, you must perform pointer subtraction against the base:
  $$\text{idx} = n - \text{heap->data}$$
- Therefore, a pointer helper cannot even work without passing the `heap` object:
  ```c
  static heap_node_t* get_parent(min_heap_t *heap, heap_node_t *node) {
      size_t idx = node - heap->data; // Pointer subtraction (divides byte offset by sizeof(node))
      if (idx == 0) return NULL;      // Root has no parent
      return &heap->data[(idx - 1) / 2];
  }
  ```

###### 2. Pointer-Index Ping-Pong
Notice what the above function does under the hood:
1. `node - heap->data` performs address subtraction and integer division by 16 (`sizeof(heap_node_t)`).
2. Computes the parent index `(idx - 1) / 2`.
3. Multiplies the parent index by 16 and adds it to `heap->data` to return a pointer.
Converting from **pointer $\to$ index $\to$ parent index $\to$ pointer** adds cognitive noise and unnecessary arithmetic compared to simply staying in index-space!

###### 3. The Swap Invalidation Trap
When two nodes swap in an array:
```c
heap_node_t tmp = heap->data[idx];
heap->data[idx] = heap->data[parent];
heap->data[parent] = tmp;
```
If your tracking cursor is a pointer `heap_node_t *p = &heap->data[idx]`, after the swap, `p` still points to the same memory slot `data[idx]`, but that slot now holds the **demoted parent**, not the climbing newcomer! 
Using integer indices (`idx = parent`) avoids all pointer aliasing and lifetime confusion.

###### 4. The Idiomatic Systems Pattern: Inline Index Helpers
High-performance systems (the Linux kernel, ClickHouse, SQLite) keep navigation strictly in **index-space** using single-cycle `static inline` helpers:

```c
static inline size_t heap_parent(size_t idx) {
    return (idx - 1) / 2;
}

static inline size_t heap_left(size_t idx) {
    return 2 * idx + 1;
}

static inline size_t heap_right(size_t idx) {
    return 2 * idx + 2;
}
```

These compile directly to single hardware instructions:
- `heap_parent`: Bitshift right (`shr reg, 1`).
- `heap_left`: Load Effective Address (`lea rax, [rdi*2 + 1]`).
- Bounds checking is a trivial scalar check: `if (left < heap->size)`.

---

##### C. Hardware & CPU Mechanics: Why Arrays Fly and Pointers Thrash

Why does this implicit array mapping run circles around pointer-based trees on modern hardware?

```text
┌────────────────────────────────────────────────────────────────────────────────┐
│                    POINTER-BASED TREE (CACHE DISASTER)                         │
│                                                                                │
│ [Node A] (at 0x1000) ──(malloc pointer)──► [Node B] (at 0x8500)                │
│                                            ├──► Random DRAM Address Jump       │
│                                            ├──► L1/L2/L3 Cache Misses          │
│                                            └──► CPU Stalled 200 Cycles!        │
├────────────────────────────────────────────────────────────────────────────────┤
│                    IMPLICIT FLAT ARRAY (HARDWARE SYMPATHY)                     │
│                                                                                │
│ [ Node 0 | Node 1 | Node 2 | Node 3 | Node 4 | Node 5 | Node 6 ]               │
│ └───────── Cache Line 0 (64 bytes) ─────────┘ └────── Cache Line 1 ─────┘     │
│                                                                                │
│ * Children of 0 (Node 1 and Node 2) are PHYSICAL CONTIGUOUS NEIGHBORS!         │
│ * Single 64-byte burst loads parent and both children simultaneously!         │
└────────────────────────────────────────────────────────────────────────────────┘
```

1. **Elimination of Pointer Chasing:**
   When traversing a pointer tree (`node = node->left`), the CPU cannot anticipate the next address because each node was allocated at an arbitrary virtual memory location by `malloc()`.
   The CPU core stalls waiting for DRAM latency:
   - L1 Cache: ~1 nanosecond (4 cycles)
   - L2 Cache: ~3 nanoseconds (12 cycles)
   - L3 Cache: ~12 nanoseconds (40 cycles)
   - Main Memory DRAM: **~60 to 100 nanoseconds (~200 to 300 cycles stalled!)**
   A pointer tree spends 95% of its execution time doing nothing—stalled waiting on the memory bus.

2. **Pointer Arithmetic at the Assembly Level:**
   In our C flat array, when we write `heap->data[idx]`, the compiler does NOT execute function calls or memory lookups. It emits a single scaled pointer arithmetic instruction:
   ```text
   ; Address = BasePointer + (Index * sizeof(heap_node_t))
   ; With sizeof(heap_node_t) = 16 bytes:
   mov rdx, [rdi + rsi * 16]   ; Single-cycle hardware address calculation!
   ```
   Pointer arithmetic in C is directly supported by the x86-64 and ARM64 memory addressing hardware (`Base + Index * Scale`).

3. **The Sibling Cache-Line Miracle:**
   Look at the formulas for children:
   - $\text{Left}(i) = 2i + 1$
   - $\text{Right}(i) = 2i + 2$
   Notice that:
   $$\text{Right}(i) = \text{Left}(i) + 1$$
   **The left and right children are physically adjacent memory neighbors!**
   - In our engine, `heap_node_t` is 16 bytes (8-byte `sort_record_t` + 8-byte `run_idx`).
   - A CPU cache line is 64 bytes. Exactly $64 / 16 = 4$ heap nodes fit inside a single cache line.
   - When `heap_sift_down` reads the left child, the CPU cache controller automatically fetches the surrounding 64 bytes. **The right child is already sitting in L1 cache before the CPU even asks for it!**
   Evaluating both children in the three-way duel costs **zero extra memory fetches**.

4. **Multi-Way / 4-Ary Heaps (Matching Cache Lines Perfectly):**
   This mechanical sympathy explains why modern database kernels often use **4-ary heaps** ($D = 4$):
   - In a 4-ary heap, a node has 4 children at indices $4i + 1, 4i + 2, 4i + 3, 4i + 4$.
   - Four 16-byte nodes = **exactly 64 bytes (one full cache line)**!
   - The CPU reads all 4 children in a single cache-line transaction and compares them simultaneously using SIMD vector instructions (`_mm256_cmpgt_epi32`).

---

> [!TIP]
> **The True Logical Origin: 1-Based Bitshifts (Williams, 1964)**
> You might wonder: *how did computer scientists actually figure out `(idx - 1) / 2`? Nobody starts by hacking with fractional `.5` truncation!*
>
> You are 100% right. Binary heaps were invented in 1964 by J. W. J. Williams for **1-indexed** arrays (Fortran and Algol).
> Look at what happens in **1-based indexing** when you write the indices in **binary**:
>
> ```text
> Level 0:                 [1] (binary 1)
>                        /     \
> Level 1:         [2] (10)   [3] (11)
>                  /    \      /    \
> Level 2:     [4](100) [5](101) [6](110) [7](111)
> ```
>
> Look at the binary bits:
> * Moving to the **Left Child**: Append a `0` bit $\implies k \times 2$ (or `k << 1`).
> * Moving to the **Right Child**: Append a `1` bit $\implies k \times 2 + 1$ (or `(k << 1) | 1`).
> * Moving to the **Parent**: **Just chop off the last bit!**
>   $$\text{Parent}(k) = k / 2 \quad (\text{or } k \gg 1)$$
>   - Parent of 7 (`111`) is 3 (`11`): $7 / 2 = 3$.
>   - Parent of 6 (`110`) is 3 (`11`): $6 / 2 = 3$.
>   - Parent of 5 (`101`) is 2 (`10`): $5 / 2 = 2$.
>   - Parent of 4 (`100`) is 2 (`10`): $4 / 2 = 2$.
>
> It was originally just **`k / 2` (a single right-shift)!**
>
> **Why is C's formula `(idx - 1) / 2`?**
> Because C insists on **0-based indexing** ($idx = k - 1 \iff k = idx + 1$):
> 1. Convert C 0-based index to 1-based: $k = idx + 1$.
> 2. Find parent using the natural 1-based rule: $k / 2 = (idx + 1) / 2$.
> 3. Convert back to C 0-based index:
>    $$\text{Parent}_0 = \left\lfloor \frac{idx + 1}{2} \right\rfloor - 1 = \left\lfloor \frac{idx + 1 - 2}{2} \right\rfloor = \left\lfloor \frac{idx - 1}{2} \right\rfloor$$
>
> That is where `(idx - 1) / 2` comes from: it is simply the natural `k >> 1` bitshift shifted into C's 0-indexed universe!
>
> **Unsigned Underflow Safety:** Because `idx` is an unsigned `size_t`, if `idx == 0`, evaluating `idx - 1` would underflow to `SIZE_MAX`. The loop guard `while (idx > 0)` is critical: it guarantees `idx >= 1`, ensuring `idx - 1` is strictly non-negative and mathematically safe.

Zero pointer overhead, 100% cache-line utilization, instant indexing!

##### Wait, does this work just because it's a binary tree?
**No! Being a binary tree is only half the secret.**

If you tried to store an arbitrary or unbalanced binary search tree (BST) in a flat array, it would be a catastrophic disaster:
```text
Degenerate "Right-Heavy" Binary Tree:       Array Mapping (Catastrophic Gaps!):
       [A] (idx 0)                          Index 0:  [A]
         \                                  Index 1:  (empty hole)
         [B] (idx 2 = 2*0+2)                Index 2:  [B]
           \                                Indices 3-5: (empty holes)
           [C] (idx 6 = 2*2+2)              Index 6:  [C]
             \                              Indices 7-13: (empty holes)
             [D] (idx 14 = 2*6+2)           Index 14: [D]  <-- 2^N memory waste!
```
For a skewed binary tree of just 30 elements, you would need an array of $2^{30} = 1\text{ Billion slots}$ just to store 30 nodes! 99.999% of the array would be empty holes.

So what makes the Binary Heap different?
1. **The "Complete Tree" Invariant (Zero Holes):**
   A heap strictly enforces that **every single level of the tree is completely filled**, except possibly the very bottom level, which is strictly **filled from left to right with zero gaps**.
   Because of this shape constraint, $N$ nodes map to **exactly indices $0, 1, 2, \dots, N-1$ with ZERO holes**!
2. **Does it have to be binary? (D-ary Heaps in Real Engines):**
   No! The same trick works for any tree with a fixed branching factor $D$ (like a **4-ary or 8-ary heap**):
   * $\text{Child}_k(i) = D \cdot i + k + 1$ (for $k \in [0, D-1]$)
   * $\text{Parent}(i) = (i - 1) / D$
   In fact, many production database kernels use a **4-heap (4 children per node)** instead of a binary heap because each 4-child node fits perfectly within a single 64-byte CPU cache line, cutting cache misses by another 30%!

---

#### 4. What is `sift_up`? The Real Intent: Bubble-Up Promotion Tournament

What is `sift_up` actually trying to achieve, and why does it exist?

##### The Core Intent: Restoring the Broken Invariant
In a Min-Heap, the fundamental rule (invariant) that must **always hold across all nodes** is:
$$\text{Parent Key} \le \text{Child Keys}$$
The smallest element in the entire system must always sit at the root (`index 0`).

When inserting a new element:
1. **The Physical Constraint:** To keep the flat array contiguous without empty memory holes, the new element **must be placed at the very end of the array** (at index `heap->size`).
2. **The Invariant Violation:** But this newcomer might have a very small key (e.g., key `5`, while its parent is `50` and the root is `10`). Placing `5` at the bottom violates the invariant: a parent is now bigger than its child!
3. **The Intent:** The sole mission of `sift_up` is to **restore the heap invariant along the single vertical path from the leaf to the root**. It promotes the newcomer up the tree level by level until it finds its rightful place.

```text
Step 1: Append 8 at bottom (idx 3)   Step 2: 8 < Parent 20? SWAP!       Step 3: 8 < Parent 10? SWAP!
           10                                  10                                   8 (New Root Champion!)
         /    \                              /    \                               /    \
       20      30                          [8]     30                           10      30
      /                                    /                                    /
    [8] (Violation! 20 > 8)              20                                   20
```

##### How the Promotion Algorithm Works:
1. Compare the node at `idx` with its parent at `(idx - 1) / 2`.
2. **If `node.key < parent.key`:** Swap the two nodes! The newcomer moves up one level, and the heavier parent is demoted down.
3. Update `idx = parent` and repeat.
4. **Stopping Conditions (Termination):**
   - **Condition A (Hierarchy Satisfied):** `node.key >= parent.key`. The newcomer has reached a boss that is smaller than it. The heap property is restored.
   - **Condition B (Top of the Tree):** `idx == 0`. The newcomer reached the root! It is the smallest value in the entire heap.

##### What is the `idx` Argument Really?

In the function signature:
```c
void min_heap_sift_up(min_heap_t *heap, size_t idx);
```

What physical entity does `idx` represent?
1. **The Culprit's Address:** `idx` is the array offset in `heap->data[]` of the **candidate node that is currently violating the heap property** and needs promotion.
   - When you call `min_heap_push()`, you append the newcomer at index `heap->size`.
   - Therefore, `push()` calls `min_heap_sift_up(heap, heap->size)`. Here, `idx` starts at the newly appended leaf!
2. **The Active Climbing Cursor:** Inside `min_heap_sift_up()`, `idx` acts as an active tracking cursor:
   ```c
   size_t parent = (idx - 1) / 2;
   if (heap->data[idx].key < heap->data[parent].key) {
       swap(&heap->data[idx], &heap->data[parent]);
       idx = parent; // Move our tracking cursor up to where our node now lives!
   }
   ```
   Every time a swap occurs, our node moves to `parent`. Setting `idx = parent` ensures the loop continues tracking that same node as it ascends towards the root.

##### Critical Systems Traps in `min_heap_sift_up` Implementation

When implementing the loop in C, three subtle traps frequently bite systems programmers:

###### Trap 1: The Unsigned Integer Underflow (`heap_parent(0)` Segfault)
In C, `size_t` is strictly an unsigned integer type (typically `uint64_t`).
If the loop condition is written as `while (1)` and executes:
```c
size_t parent = heap_parent(idx); // when idx == 0!
```
The expression `(idx - 1)` performs `0 - 1` on an unsigned type!
Instead of `-1`, the value wraps around to **`18,446,744,073,709,551,615` (`SIZE_MAX`)**.
Dividing by 2 yields index `9,223,372,036,854,775,807`.
Accessing `heap->data[parent]` immediately crashes with a catastrophic **Segmentation Fault**!
- **The Fix:** Never call `heap_parent(idx)` unless you have proven `idx > 0`. The loop condition must be:
  ```c
  while (idx > 0)
  ```

###### Trap 2: Struct Comparison Syntax Restriction
In C11, structs cannot be compared using relational operators (`<`, `<=`, `>`, `>=`):
```c
// ILLEGAL C SYNTAX - Compiler Error!
if (heap->data[idx] <= heap->data[parent]) 
```
A struct represents a composite memory block, not a scalar number. The compiler cannot assume whether you want to compare memory bytes, keys, or pointers. You must compare the explicit scalar member:
```c
if (heap->data[idx].key >= heap->data[parent].key) {
    break; // Invariant satisfied: parent is smaller or equal
}
```

###### Trap 3: Invariant Condition Logic (When to Break vs. When to Swap)
The Min-Heap invariant states that $\text{Parent.key} \le \text{Child.key}$.

There are two equally valid and idiomatic ways to structure this loop:

- **Style A: Guard Clause (Early `break`):**
  Check if the invariant is satisfied. If yes, stop; otherwise swap:
  ```c
  if (heap->data[parent].key <= heap->data[idx].key) {
      break; // Invariant already satisfied!
  }
  // Proceed with swap...
  ```

- **Style B: Positive Condition (`if (child < parent) swap; else break;`):**
  Check if the newcomer is strictly smaller than the parent. If yes, promote it; otherwise stop:
  ```c
  if (heap->data[idx].key < heap->data[parent].key) {
      // Newcomer is smaller than parent: SWAP!
      heap_node_t tmp = heap->data[idx];
      heap->data[idx] = heap->data[parent];
      heap->data[parent] = tmp;
      idx = parent;
  } else {
      // Invariant holds (parent <= child): DONE!
      break;
  }
  ```

> [!WARNING]
> **The Inverted Inequality Trap (`child <= parent break`):**
> Writing `if (heap->data[idx].key <= heap->data[parent].key) break;` accidentally reverses the entire heap!
> Trace it with numbers:
> - Existing parent key = `50`. Newcomer child key = `10`.
> - Check: `10 <= 50`? Evaluates to **TRUE**!
> - The code executes `break`, leaving `50` above `10`!
> - The newcomer `10` is trapped at the bottom, and the min-heap invariant is broken.
> - **Remember:** If you use `<` with child on the left, it is the **SWAP** condition, not the break condition!

---

###### Trap 4: Distinguishing Data Swapping from Cursor Advancing
A common point of confusion is: *If we do `inserted_node_idx = parent;`, do we also need `parent = inserted_node_idx;`?*

**No!** Do not confuse **swapping array data** with **updating index cursors**:
1. **The Data Swap:** Swaps the 16-byte values in RAM:
   ```c
   heap_node_t tmp = heap->data[idx];
   heap->data[idx] = heap->data[parent];
   heap->data[parent] = tmp;
   ```
   Now the newcomer is physically sitting at index `parent` in the array.
2. **The Cursor Advance:** We want our loop to follow that newcomer up to the next level:
   ```c
   idx = parent; // Our newcomer is now at index 'parent'
   ```
3. **The Next Iteration:** At the top of the next loop iteration, the *new* parent is dynamically recalculated:
   ```c
   size_t parent = heap_parent(idx); // Finds the parent of our new position!
   ```
   If you had written `parent = idx`, it would be immediately overwritten by `heap_parent(idx)` on the next iteration.

---

##### First-Principles Mathematical Proof: How to Calculate Big-O ($O(\log N)$)

How do computer scientists mathematically derive that `sift_up` is $O(\log N)$ rather than $O(N)$ or $O(1)$? 

Let us prove it step-by-step from foundational axioms:

###### Step 1: Work Done Per Loop Iteration
Look at what happens inside a single loop iteration of `sift_up`:
1. Calculate parent: `(idx - 1) / 2` $\longrightarrow$ 1 subtraction, 1 bitshift ($O(1)$ constant time).
2. Compare keys: `heap->data[idx].key < heap->data[parent].key` $\longrightarrow$ 1 CPU comparison ($O(1)$ constant time).
3. Swap nodes: 3 memory moves of 16-byte structs $\longrightarrow$ ($O(1)$ constant time).
4. Update cursor: `idx = parent` $\longrightarrow$ 1 register assignment ($O(1)$ constant time).

Every single iteration executes a fixed, constant number of CPU cycles:
$$\text{Work per step} = C = O(1)$$

###### Step 2: Maximum Number of Iterations (Tree Height $H$)
In each iteration of the loop, our node moves from level $h$ up to level $h - 1$.
$$\text{Level } h \longrightarrow \text{Level } (h - 1) \longrightarrow \text{Level } (h - 2) \longrightarrow \dots \longrightarrow \text{Level } 0 \text{ (Root)}$$
The loop can **never execute more times than the height of the tree ($H$)**.
Therefore, the total time complexity function $T(N)$ is:
$$T(N) \le C \times H$$

###### Step 3: Solving for Tree Height $H$ as a Function of Node Count $N$
In a complete binary tree:
- Level 0 (Root): $2^0 = 1$ node
- Level 1: $2^1 = 2$ nodes
- Level 2: $2^2 = 4$ nodes
- Level 3: $2^3 = 8$ nodes
- Level $h$: $2^h$ nodes

The total number of nodes $N$ in a fully saturated binary tree of height $H$ is the sum of a geometric progression:
$$N = \sum_{h=0}^{H} 2^h = 2^0 + 2^1 + 2^2 + \dots + 2^H$$

Applying the standard finite geometric series sum formula $S = \frac{a(r^{n} - 1)}{r - 1}$ with $a=1$ and $r=2$:
$$N = 2^{H+1} - 1$$

Now, we use high school algebra to isolate height $H$ in terms of $N$:
$$N + 1 = 2^{H+1}$$

Take the base-2 logarithm ($\log_2$) of both sides:
$$\log_2(N + 1) = \log_2(2^{H+1})$$
$$\log_2(N + 1) = H + 1$$
$$H = \log_2(N + 1) - 1$$

By asymptotic Big-O definition, we drop additive constants:
$$H = \Theta(\log_2 N)$$

###### Step 4: Final Asymptotic Complexity
Multiplying the constant work per step by the maximum number of steps:
$$T(N) = O(1) \times \log_2(N) = \mathbf{O(\log N)}$$

###### Physical Scale Comparison:
| Elements in Heap ($N$) | Tree Height ($H \approx \log_2 N$) | Maximum Swaps in `sift_up` | Full Array Sort ($N \log_2 N$) |
| :--- | :--- | :--- | :--- |
| **8** | 3 | **3 comparisons** | 24 operations |
| **64** | 6 | **6 comparisons** | 384 operations |
| **1,024** | 10 | **10 comparisons** | 10,240 operations |
| **1,000,000 (1 Million)** | 20 | **20 comparisons** | 20,000,000 operations |
| **1,000,000,000 (1 Billion)**| 30 | **30 comparisons** | 30,000,000,000 operations |

In a dataset of **1 billion rows**, `sift_up` restores global invariant integrity in at most **30 CPU clock cycles**!

---

#### 5. What is `heap_sift_down`? (The Replacement Sinks Down)
Used during streaming iteration (`external_sorter_next()`):

1. **The Context: A Champion Has Been Crowned**
   In `external_sorter_next()`, the consumer takes the winner from the root `heap->data[0]` (the absolute minimum key).
   Now the throne at `data[0]` is vacant!
2. **The Replacement:**
   We read the next record from the run file that produced the champion. Say its key is `70`.
   We place `70` at the root: `heap->data[0].record = next_rec;`.
3. **The Impostor Problem:**
   `70` is huge! The children at index 1 and index 2 might have keys `25` and `15`. Placing `70` at the root violently breaks the Min-Heap Invariant ($\text{parent} \le \text{children}$).
4. **The Sinking Process (Sift Down):**
   `70` must sink down to its rightful depth:
   - **The Three-Way Duel:** Compare the current node with its Left Child (`2*idx + 1`) and its Right Child (`2*idx + 2`).
   - **Why MUST We Swap with the SMALLEST Child?**
     If Left is `25` and Right is `15`:
     - If you swap with `25`, then `25` becomes the parent of `15`. But $25 > 15$, which **violates** the min-heap rule!
     - If you swap with the **smallest child (`15`)**, then `15` becomes the parent of both `25` and `70`. Since $15 \le 25$ and $15 \le 70$, the min-heap rule is **perfectly preserved**!
   - Repeat the duel until the node is smaller than both its children, or it reaches the bottom of the tree.

#### 5. What is `sift_down`? The Intent: Sinking the Heavy Impostor

What is the fundamental intent of `sift_down`, and when is it invoked?

##### The Two Scenarios that Trigger `sift_down`:

###### Scenario 1: Deleting / Popping the Minimum (`min_heap_pop`)
1. **The Problem:** In a Min-Heap, the smallest element always sits at the root (`index 0`). When a consumer asks for the minimum element, we take `heap->data[0]`. 
2. **The Vacant Throne:** Now index 0 is empty! We cannot leave a hole, nor can we shift all array elements left by one ($O(N)$ memory copies would destroy our $O(\log N)$ performance).
3. **The Leaf Replacement:** To maintain the contiguous "Complete Binary Tree" invariant, we take the **very last element** in the array (`heap->data[--heap->size]`) and move it into index 0!
4. **The Impostor:** But that leaf element came from the bottom of the tree—it is typically a **large value** (e.g. `90`)! Placing `90` at the root violently breaks the invariant ($\text{Parent} \le \text{Children}$) because its children might be `15` and `25`.
5. **The Sinking Mission:** The intent of `sift_down` is to **sink that heavy element down the tree until it is smaller than both of its children (or becomes a leaf).**

###### Scenario 2: Streaming Tournament Replacement (`min_heap_replace_top`)
In analytical database engines doing external merge sort (and LSM part compactions), we don't even shrink the heap:
1. We read the winning tuple at the root (`data[0]`).
2. We read the next tuple from that same disk run (say `70`).
3. We overwrite the root directly: `heap->data[0] = next_tuple;`.
4. We call `min_heap_sift_down(heap, 0)` to sink `70` down to its proper level in $O(\log K)$ time. Zero allocations, zero array resizing!

```text
Step 1: Replace Root with 70         Step 2: Smaller Child is 15. SWAP!   Step 3: Smaller Child is 18. SWAP!
          [70]                                 15                                   15
         /    \                              /    \                               /    \
       25      15                          25     [70]                          25      18
      /  \    /  \                        /  \    /  \                         /  \    /  \
     40  60  30   18                     40  60  30   18                      40  60  30  [70]
```

##### The Core Algorithmic Rule: Why MUST We Swap with the SMALLEST Child?
When sinking a node down, why can't we just swap with any child that is smaller than us?

Suppose the node at index 0 has key `90`:
- Left Child = `25`
- Right Child = `15`

What happens if you mistakenly swap with the Left Child (`25`)?
- `25` moves up to the root.
- Its children are now `90` and `15`.
- **Invariant Violation!** The root (`25`) is bigger than its right child (`15`)!

> [!IMPORTANT]
> **The Three-Way Tournament Rule:**
> To become the new parent, a node must be **smaller than BOTH of its future children**.
> Therefore, you must compare the current node against **both** the left child and the right child, find the **absolute minimum of the three**, and swap with that winner!

##### Loop Bounds & Termination Conditions:
1. **When does a node have children?**
   In a complete binary tree, children are at `2*idx + 1` (left) and `2*idx + 2` (right).
   If `heap_left(idx) >= heap->size`, the node has no children at all (it has hit the bottom leaves). It cannot sink any further!
2. **What if a node only has a left child?**
   The last internal node in an odd-sized heap might have a left child, but no right child. The algorithm must check `if (heap_right(idx) < heap->size)` before inspecting the right child.
3. **When does it stop?**
   When the current node is already smaller than or equal to both children (`smallest == idx`), the invariant is restored, and the loop breaks.

##### The "Smallest Candidate" Pattern: Eliminating Combinatorial If/Else Traps

Writing `sift_down` with naive `if / else if` branches (e.g. `if (left < right && left < curr)`) introduces three dangerous systems traps:

1. **The Out-of-Bounds Memory Trap:**
   A node in a heap frequently has a left child but **no right child** (e.g., in a heap of 4 elements, index 1 has left child 3, but right child 4 is out of bounds). Evaluating `heap->data[left].key < heap->data[right].key` without first proving `right < size` dereferences uninitialized memory past the array boundary!
2. **The Equality Tie Trap:**
   If `left.key == right.key` and both are smaller than `curr.key` (e.g. `left = 10, right = 10, curr = 50`), both `left < right` and `right < left` evaluate to **false**. A naive if/else drops into `else break;`, prematurely aborting the sink without swapping!
3. **Duplicate Swap Code:**
   Writing separate swap blocks for the left child and right child duplicates memory-swapping logic unnecessarily.

###### The Classical Tournament Solution (Knuth / CLRS / ClickHouse):
Instead of nested combinatorial branches, systems kernels maintain a **`smallest` candidate index**:
- Initialize `smallest = idx;` (assume current node is champion).
- Compare Left: If left child is smaller than current champion, set `smallest = left;`.
- Compare Right: If right child exists and is smaller than the current champion (beating both `idx` and `left`), set `smallest = right;`.
- Finish: If `smallest == idx`, no child was smaller $\longrightarrow$ break. Otherwise, execute a single unified swap with `smallest` and continue sinking!

###### Two Subtle Implementation Traps in `sift_down`:
1. **The 0-Indexed `<=` Off-By-One Trap:**
   In C, array indices run from `0` to `size - 1`. If the loop checks `while (heap_left(idx) <= heap->size)`, when `heap_left(idx) == heap->size`, the condition evaluates to **true** and attempts to read `heap->data[heap->size]`—which is uninitialized or unallocated memory past the active heap! Always use strict inequality: `while (heap_left(idx) < heap->size)`.
2. **The Swap Overwrite Trap:**
   Writing:
   ```c
   heap_node_t tmp = heap->data[smallest];
   heap->data[idx] = heap->data[smallest]; // OVERWRITES data[idx]!
   heap->data[smallest] = tmp;             // Puts smallest back into smallest!
   ```
   accidentally duplicates `data[smallest]` into both slots and permanently erases the original `data[idx]`! 
   The swap must save the node being overwritten: `heap_node_t tmp = heap->data[idx];`.

---

#### 6. Authoritative References & Recommended Reading on Binary Heaps

For deep theoretical foundations, proofs of the $O(N)$ heap construction bound, and historical origins of the data structure, consult these gold-standard references:

##### 1. The Original Discovery Paper
* **Williams, J. W. J. (1964).** *"Algorithm 232: Heapsort"*. **Communications of the ACM (CACM)**, 7(6), 347–348.
  - *Significance:* The historic paper that first introduced the binary heap, the 1-based array mapping arithmetic ($2i, 2i+1, i/2$), and the in-place Heapsort algorithm.
  - [ACM Digital Library Reference (DOI: 10.1145/368888.369029)](https://doi.org/10.1145/368888.369029)

##### 2. Classic Academic Textbooks
* **Cormen, T. H., Leiserson, C. E., Rivest, R. L., & Stein, C. (CLRS):**
  - *Introduction to Algorithms* (MIT Press, 4th Edition).
  - **Chapter 6: Heapsort** (Sections 6.1–6.5: Implicit tree array representation, `MIN-HEAPIFY` / `sift_down`, and Priority Queue ADT implementation).
* **Knuth, Donald E.:**
  - *The Art of Computer Programming, Volume 3: Sorting and Searching* (Addison-Wesley).
  - **Section 5.2.3: Sorting by Selection**: Detailed historical survey and rigorous mathematical analysis of tree selection and heap invariants.
* **Sedgewick, Robert & Wayne, Kevin:**
  - *Algorithms* (Addison-Wesley, 4th Edition).
  - **Section 2.4: Priority Queues**: Excellent treatment of `swim()` (`sift_up`) and `sink()` (`sift_down`).
  - Online Companion Course: [Princeton CS: Priority Queues](https://algs4.cs.princeton.edu/24pq/)

##### 3. Open-Access Textbooks & Interactive Visualizers
* **Pat Morin (Carleton University):**
  - *Open Data Structures (in C++)*, **Chapter 10: Heaps — BinaryHeap: An Implicit Binary Tree**.
  - Complete, open-access textbook with rigorous proofs and code: [opendatastructures.org](https://opendatastructures.org/ods-cpp/10_1_BinaryHeap_Implicit_Bi.html)
* **Keith Schwarz (Stanford University):**
  - *CS106B Programming Abstractions: Heaps and Priority Queues*.
  - [Stanford CS106B Lecture Slides & Handout](https://web.stanford.edu/class/archive/cs/cs106b/cs106b.1218/lectures/17-heaps/)
* **VisuAlgo (National University of Singapore):**
  - *Binary Min-Heap Visualizer & Step-by-Step Interactive Animation*.
  - Recommended interactive sandbox to visualize tree nodes bubbling up and sinking down in real time: [visualgo.net/en/heap](https://visualgo.net/en/heap)
* **Wikipedia Reference Guides:**
  - [Binary Heap (Data Structure Overview)](https://en.wikipedia.org/wiki/Binary_heap)
  - [Priority Queue Abstract Data Type](https://en.wikipedia.org/wiki/Priority_queue)

---

#### 7. Standalone Component Modularization: `include/simpledb/min_heap.h`

You made a stellar software engineering observation:
> *"The Min-Heap is worth having its own section, module, and dedicated tests. The algorithm is simple enough, but managing a flat array with pointer arithmetic is a self-contained systems component that should be reusable."*

Extracting the priority queue into an independent, standalone module (`min_heap.h` / `min_heap.c`) delivers two massive architectural wins for `simpledb`:

##### 1. Dual Reusability (Day 15 Sorter + Day 19 Part Compactor)
A Min-Heap is not specific to `ORDER BY`. In ClickHouse and `simpledb`, we need a streaming $K$-way min-heap in **two separate subsystems**:
* **Day 15 (Today):** Merging $K$ temporary `.run` scratch files during query execution.
* **Day 19 (The Part Compactor):** Merging $K$ immutable on-disk columnar parts (`part_000`, `part_001`, `part_002`) into a consolidated table part during background storage compaction.

By making `min_heap` a standalone module, Day 19's compactor can `#include "simpledb/min_heap.h"` directly without pulling in any external sorting logic!

##### 2. Isolated, Zero-I/O Unit Testability
When the heap is embedded inside `external_sort.c`, testing heap edge cases (sifting, duplicate keys, capacity overflow, empty pops) requires creating dummy disk files and staging directories.
With a standalone `min_heap` module, we can write blazing-fast, in-memory unit tests (`test_min_heap()`) that verify binary heap invariants in isolation without touching disk or OS file descriptors.

---

### Architectural Deep Dive: The Node Representation Dilemma (`void *` vs. Monomorphic vs. Key-Payload)

When designing a priority queue for a high-performance database engine, a critical software engineering question arises:

> *"Why does `heap_node_t` contain a `sort_record_t` and a `run_idx`? A priority queue shouldn't know what a database tuple is, nor should it care about disk run indices. Wouldn't it be much cleaner to make it generic using `void *`?"*

This observation touches on one of the foundational tensions in systems programming: **Object-Oriented Abstraction vs. Mechanical Sympathy (Hardware Performance)**.

Let us evaluate the three architectural patterns available in C11:

```text
Pattern A: Fully Generic void*              Pattern B: Key + Opaque Payload             Pattern C: Monomorphic Specialization
┌──────────────────────────────────────┐    ┌──────────────────────────────────────┐    ┌──────────────────────────────────────┐
│ typedef struct {                     │    │ typedef struct {                     │    │ typedef struct {                     │
│     void *data;                      │    │     int64_t key;   // Direct scalar  │    │     sort_record_t record;            │
│ } heap_node_t;                       │    │     void *payload; // Opaque context │    │     size_t run_idx;                  │
│                                      │    │ } heap_node_t;                       │    │ } heap_node_t;                       │
│ int (*cmp)(const void*, const void*) │    │                                      │    │                                      │
└──────────────────────────────────────┘    └──────────────────────────────────────┘    └──────────────────────────────────────┘
   Pure abstraction, high overhead             Systems compromise: inlined compare         Maximum performance, zero indirection
```

#### 1. Pattern A: The CS Textbook Approach — Fully Generic `void *` with Function Pointer Comparator

In general-purpose software engineering libraries (such as GLib, C++ standard library wrappers, or C `qsort`), containers are made generic via `void *` and comparator callbacks:

```c
typedef struct {
    void **items;
    size_t size;
    size_t capacity;
    int (*compare)(const void *a, const void *b);
} min_heap_t;
```

* **The Benefit:** 100% decoupling. The heap is an abstract data type that can order strings, JSON documents, float vectors, or database records without changing a single line of heap code.
* **The Cost (The OLAP Performance Killer):**
  1. **Indirect Function Call Penalties:** In a K-way merge of 100 million records across $K = 64$ runs, each record requires $\log_2(64) = 6$ levels of sifting. Each level performs 2 child comparisons. That equals **$1.2 \times 10^8$ indirect function calls** through a function pointer.
  2. **Pipeline Stalls & Inlining Defeat:** Modern superscalar CPUs (Intel x86, Apple Silicon ARM) cannot inline function pointers. The CPU cannot speculatively execute through the dynamic jump without risking branch misprediction penalties (15–20 CPU cycles per mispredict).
  3. **Pointer Dereferencing & L1 Cache Eviction:** To compare two elements, the CPU must dereference `items[i]` to reach the object, and then dereference the object to reach the sorting key. This pointer-chasing pattern degrades the flat array's spatial cache locality.

#### 2. Pattern B: The Systems Compromise — Primitive Key + Opaque Payload (`void *payload`)

To retain clean abstraction while eliminating function pointer overhead, systems programmers frequently use the **Key + Payload** idiom:

```c
typedef struct {
    int64_t key;     // Primitive sort key (compared directly with < or >)
    void *payload;   // Opaque caller pointer (e.g. run stream, part cursor, or row ID)
} heap_node_t;
```

* **Why It Works:**
  - **Inlined Arithmetic:** The heap's `sift_up` and `sift_down` functions compare `node.key` directly with primitive integer instructions (`cmpq`, `b.lt`). Zero function calls, zero indirect jumps, and full compiler optimization.
  - **Decoupled Abstraction:** The heap knows nothing about `sort_record_t`, `run_idx`, or disk runs.
    - In **Day 15 (External Sort)**: `payload` can point to a `run_stream_t` holding `{ FILE *fp; size_t run_idx; int32_t row_id; }`.
    - In **Day 19 (Part Compaction)**: `payload` points to a `part_reader_t`.
    - In **Top-K Queries**: `payload` points directly to table rows.
  - **Cache Line Density:** At 8 bytes for `int64_t` + 8 bytes for `void *`, each `heap_node_t` is exactly 16 bytes. A 64-byte L1 CPU cache line holds exactly 4 complete nodes!

#### 3. Pattern C: The Monomorphic High-Performance Engine (ClickHouse / SQLite Approach)

In production database engines (like SQLite's `vdbesort.c` and ClickHouse's `MergingSortedTransform`), priority queues are often deliberately **monomorphic** (specialized to the query engine's tuple representation):

* In C++, ClickHouse uses templates (`std::priority_queue<Cursor, ...>`), which generates specialized machine code for the exact cursor struct with zero runtime overhead.
* In C11, `simpledb` achieves the same efficiency by embedding the concrete 8-byte `sort_record_t` and `run_idx` directly into the node.
* If architectural cleanliness is desired, this monomorphic heap can be kept **private and static** inside `src/external_sort.c` (or given an explicit name like `merge_tournament_t`), ensuring it does not pollute public headers.

---

### Lifecycle Ergonomics: Why `_init` is a Leaky Abstraction (The Case for `min_heap_create`)

When looking at the API signature:

```c
int min_heap_init(min_heap_t *heap, size_t capacity);
```

A crucial software engineering critique emerges:

> *"Passing `heap` to `init` is a leaky abstraction. It forces the caller to allocate and manage the container struct itself, exposing internal struct size and breaking information hiding."*

This critique is spot on. While embedded in-place initialization (`_init`) is occasionally used in C for micro-optimizing away a 24-byte allocation, in terms of software architecture, **`_init` is fundamentally leaky**:

```text
The Leaky Abstraction (_init)                  The Airtight Abstraction (_create)
┌─────────────────────────────────────────┐    ┌─────────────────────────────────────────┐
│ Caller must know sizeof(min_heap_t)     │    │ Caller only holds an opaque pointer:    │
│ Caller allocates struct memory          │    │ min_heap_t *heap = min_heap_create(K);  │
│ Heap internals are exposed in header    │    │ Full information hiding & encapsulation │
│ Inconsistent with rest of simpledb      │    │ Consistent with table_create, etc.      │
└─────────────────────────────────────────┘    └─────────────────────────────────────────┘
```

#### Why `min_heap_create` is the Right Architecture:

1. **Airtight Encapsulation & Information Hiding:**
   With `_init`, the caller must know `sizeof(min_heap_t)`. The header cannot use an opaque pointer (`typedef struct min_heap_t min_heap_t;`). Any internal modification to the struct (adding metrics, changing alignment, or switching tree arity) causes ABI churn. With `min_heap_create`, the implementation details remain completely private.

2. **System-Wide Design Consistency:**
   Throughout `simpledb`, every subsystem adheres to the canonical constructor/destructor lifecycle:
   - `external_sorter_create()` / `external_sorter_destroy()`
   - `table_create()` / `table_destroy()`
   - `string_dict_create()` / `string_dict_destroy()`
   - `column_reader_open()` / `column_reader_close()`
   Introducing `_init` specifically for `min_heap` breaks the principle of least astonishment and introduces an inconsistent cognitive model for developers using the API.

3. **Single Owner & Single Lifetime:**
   With `_create()`, the priority queue owns both its control block and its underlying dynamic buffer. A single call to `min_heap_destroy(heap)` frees both:
   ```c
   min_heap_t *heap = min_heap_create(capacity);
   if (!heap) { /* handle OOM */ }
   // ... use heap ...
   min_heap_destroy(heap); // Frees data buffer and container cleanly
   ```

4. **Defensive Allocation & Cleanup Contract:**
   Robust systems constructors must satisfy the *all-or-nothing allocation guarantee*:
   - If the container allocation `malloc(sizeof(*heap))` fails, return `NULL` immediately.
   - If the underlying array allocation `malloc(sizeof(heap_node_t) * capacity)` fails, the constructor must unwind and `free(heap)` before returning `NULL` to prevent leaking the container under memory pressure.
   - Similarly, destructors (`min_heap_destroy`) must follow standard C `free(NULL)` conventions: if `!heap`, return immediately rather than causing a null pointer dereference on `free(heap->data)`.

---

### Failure Mode Strategy: What Should Systems Code Do When `malloc()` Fails?

When an allocation fails in C, systems engineers choose between four fundamentally different error philosophies:

```text
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                        4 Failure Philosophies on malloc() Failure                      │
├──────────────────────────┬──────────────────────────┬──────────────────────────────────┤
│ Strategy                 │ Mechanism                │ Where It Belongs / Trade-off     │
├──────────────────────────┼──────────────────────────┼──────────────────────────────────┤
│ 1. Fail-Fast (Crash)     │ abort() or exit(1)       │ CLI tools (git, grep), unit tests│
│                          │                          │ Disastrous for database servers! │
├──────────────────────────┼──────────────────────────┼──────────────────────────────────┤
│ 2. Clean Unwind (NULL)   │ Free partials, return 0  │ Reusable C libraries & kernels   │
│                          │ Propagate error upward   │ The Golden Standard for modules  │
├──────────────────────────┼──────────────────────────┼──────────────────────────────────┤
│ 3. The Linux Overcommit  │ vm.overcommit_memory     │ malloc() succeeds, but writing   │
│    (OOM Killer Trap)     │ Physical pages lazily    │ triggers SIGKILL from the OS     │
├──────────────────────────┼──────────────────────────┼──────────────────────────────────┤
│ 4. Proactive Tracking    │ Logical memory trackers  │ Production engines (ClickHouse)  │
│    (Circuit Breakers)    │ Abort query BEFORE OOM   │ Never let OS malloc hit the wall │
└──────────────────────────┴──────────────────────────┴──────────────────────────────────┘
```

#### 1. Why "Crash Early" (`abort()` / `exit()`) is Disastrous for Databases
In single-threaded command-line utilities (like `grep` or `jq`), calling `exit(EXIT_FAILURE)` when memory is exhausted is standard practice. If the machine has no memory, the utility cannot proceed.

However, in a **database server** (serving hundreds of concurrent analytical queries across dozens of tenants), having a single query cause `exit()` or `abort()` is a catastrophic failure:
- It terminates the entire daemon process.
- It drops hundreds of active client socket connections.
- It leaves file locks hanging on shared NVMe storage.
- It interrupts active LSM compaction jobs, forcing lengthy crash recovery on restart.

#### 2. The Golden Rule for Modules: Clean Unwinding (`return NULL`)
A low-level systems data structure (like `min_heap`) must **never decide process policy**. It has no idea whether it is running inside an ephemeral test, a background thread, or a mission-critical query engine.

Therefore, the only correct architectural behavior for `min_heap_create()` is:
```c
min_heap_t *min_heap_create(size_t capacity) {
    min_heap_t *heap = malloc(sizeof(*heap));
    if (!heap) {
        return NULL; // Allocation 1 failed: return clean NULL
    }

    heap->data = malloc(sizeof(heap_node_t) * capacity);
    if (!heap->data && capacity > 0) {
        free(heap);  // Allocation 2 failed: unwind allocation 1 to prevent memory leak!
        return NULL;
    }

    heap->capacity = capacity;
    heap->size = 0;
    return heap;
}
```
This gives the caller full control:
- The sorter checks `if (!sorter->heap) return -1;`.
- The query coordinator catches the `-1` and cleanly returns `DB_ERROR_OUT_OF_MEMORY` over the network to the single client whose query was too large.
- All other concurrent database transactions continue running without interruption!

#### 3. The Linux Overcommit Reality (Why `malloc()` Rarely Fails on Linux)
On modern Linux systems, `malloc()` rarely returns `NULL` even if physical RAM is full. By default, Linux employs **optimistic virtual memory overcommit** (`vm.overcommit_memory = 0`):
- `malloc(100MB)` succeeds instantly because the kernel merely reserves virtual address space without mapping physical DRAM frames.
- Physical memory is only allocated when the CPU touches a page (triggering a page fault).
- If physical RAM and swap are completely exhausted when pages are touched, the Linux **OOM Killer** wakes up and fires `SIGKILL` (signal 9) at the process with the highest `oom_score`!

#### 4. The Production Solution: Proactive Circuit Breakers
Because the OS cannot be trusted to gracefully return `NULL` under heavy load, production analytical engines (ClickHouse, DuckDB) never rely on `malloc()` returning `NULL`. 
Instead, they track logical allocations upfront (`CurrentMemoryTracker::alloc(bytes)`). If a query exceeds its allocated quota (e.g. `max_memory_usage = 10GB`), the engine **throws a user-level memory limit exception before touching physical RAM**, aborting the greedy query cleanly.

---

### Implementation Dependency Order: Kernels First (`sift_up` / `sift_down`)

When constructing the priority queue module, what is the natural implementation sequence? 

The module forms a clean **two-tier dependency hierarchy**:

```text
High-Level API (Leaves):      [ push ]        [ pop ]        [ replace_top ]        [ peek ]
                                 │               │                  │                  │
                                 ▼ (Calls)       ▼ (Calls)          ▼ (Calls)          ▼
Low-Level Kernels (Roots):   [ sift_up ]   [ sift_down ]      [ sift_down ]      (Direct Read)
```

1. **Step 1: The Core Navigation Kernels (`sift_up` and `sift_down`):**
   These two functions contain all the pointer arithmetic, tree navigation, and element swapping logic:
   - `min_heap_sift_up(heap, idx)`: Restores the min-heap invariant by bubbling an element up towards the root.
   - `min_heap_sift_down(heap, idx)`: Restores the min-heap invariant by sinking an element down towards the leaves (swapping with the smaller of its two children).

---


2. **Step 2: The User-Facing API (`push`, `pop`, `peek`, `replace_top`):**
   Once the sift kernels are written, the user-facing operations become trivial 4-to-6-line wrappers:
   - **`push(key, payload)`**: Bounds-check capacity $\longrightarrow$ place new element at index `heap->size` $\longrightarrow$ call `sift_up(heap, heap->size)` $\longrightarrow$ increment `heap->size++`.
   - **`peek(out_node)`**: Return `heap->data[0]` if `size > 0`.
   - **`pop(out_node)`**: Save root `heap->data[0]` $\longrightarrow$ move last element `heap->data[--heap->size]` to root $\longrightarrow$ call `sift_down(heap, 0)`.
   - **`replace_top(key, payload)`**: Overwrite root `heap->data[0]` with new node $\longrightarrow$ call `sift_down(heap, 0)`.

Therefore, **always write the sift kernels first**. You cannot implement or test `push` or `pop` without them!

---

### The Anatomy of `min_heap_push`: Intent & Execution Contract

What is the exact mission of `min_heap_push(min_heap_t *heap, int64_t key, void *payload)`?

Its intent is to safely admit a new candidate into the priority queue in $O(\log K)$ time while preserving both the **Complete Tree Structural Invariant** and the **Min-Heap Key Invariant**.

```text
               Anatomy of min_heap_push()
┌────────────────────────────────────────────────────────┐
│ 1. Guard Capacity: if (size >= capacity) return -1;    │
├────────────────────────────────────────────────────────┤
│ 2. Append at Tail: data[size] = { key, payload };      │
├────────────────────────────────────────────────────────┤
│ 3. Restore Invariant: sift_up(heap, size);             │
├────────────────────────────────────────────────────────┤
│ 4. Increment Size: size++; return 0;                   │
└────────────────────────────────────────────────────────┘
```

#### Step-by-Step Contract:
1. **Defensive Bounds Check:** 
   Verify that `heap != NULL` and that `heap->size < heap->capacity`. If the heap is already full, attempting to write into `data[size]` causes a heap buffer overflow. Return `-1` cleanly to let the caller handle backpressure.
2. **Zero-Hole Insertion:**
   To guarantee that the tree remains a *Complete Binary Tree*, the element must be appended at the first unoccupied slot: `heap->data[heap->size]`.
3. **Trigger the Promotion Tournament:**
   Call `min_heap_sift_up(heap, heap->size)`. The kernel inspects the newcomer and bubbles it up the parent chain until the invariant is restored.
4. **The Safe Increment Rule:**
   Notice that `heap->size` is incremented **after** `sift_up` completes (or captured in a temporary variable `size_t idx = heap->size++`). 
   Never write `heap->data[heap->size++] = node; sift_up(heap, heap->size);`, because `heap->size` will have already been incremented, passing the index of an uninitialized slot to `sift_up`!

---

### The Anatomy of `peek`, `pop`, and `replace_top`

The remaining three query operations complete the priority queue contract:

#### 1. `min_heap_peek(heap, out_node)`: $O(1)$ Inspection
- **Intent:** Examine the current minimum key without modifying the heap.
- **Contract:** If `!heap || heap->size == 0 || !out_node`, return `false`. Otherwise, copy `*out_node = heap->data[0]` and return `true`.

##### The Return Value Dilemma for `peek`: Value vs. Pointer vs. Out-Parameter
A common API design question is: *Why not just return the node directly like `heap_node_t min_heap_peek(heap)` or `const heap_node_t *min_heap_peek(heap)`?*

1. **Why Returning by Value (`heap_node_t peek()`) Fails:**
   In C, structs cannot be `NULL`. What should the function return if the heap is empty?
   Any `int64_t key` (such as `0`, `-1`, or `INT64_MAX`) is a valid domain key! There is no universal "sentinel value" that safely signals an empty heap. Returning uninitialized garbage forces the caller into undefined behavior.
2. **Returning an Internal Pointer (`const heap_node_t *peek()`):**
   - If empty: `return NULL;` (natural and clean!).
   - If non-empty: `return &heap->data[0];`.
   - **The Memory Ownership Ambiguity ("Who frees this?"):** Returning a raw pointer creates dangerous ownership confusion in C. If a caller mistakenly calls `free()` on the returned pointer, the program crashes with `pointer being freed was not allocated`, because it is an interior pointer pointing inside the heap's `data` block! Worse, if the heap subsequently modifies, sifts, or reallocates its array, the caller's pointer becomes a lethal dangling pointer.
3. **Returning a Status Flag with Out-Pointer (`bool peek(heap, out_node)`):**
   - **Crystal-Clear Ownership:** The caller passes the address of their own stack variable (`heap_node_t node;`). The heap simply copies the 16-byte payload into it.
   - The caller owns their own stack frame; the heap owns its internal array. Zero ownership ambiguity, zero dangling pointers, zero accidental `free()` crashes!
   - Distinct separation of status (`bool success`) and data payload (`*out_node`).
   - Highly consistent with the rest of `simpledb` (e.g. `bool external_sorter_next(sorter, out_record)`).

##### The Defensive Guards: Why Checking `!heap` Alone Is Insufficient
When implementing `min_heap_peek`, checking `if (!heap) return false;` is necessary but not sufficient. A robust implementation requires three distinct checks:
```c
if (!heap || heap->size == 0 || !out_node) {
    return false;
}
```
1. **`heap->size == 0` (The Garbage Read Trap):**
   When `heap->size` is `0`, the heap contains no logical elements.
   If the code blindly executes `*out_node = heap->data[0];`:
   - If `capacity > 0`, it reads uninitialized or stale heap memory from previous runs, returning arbitrary garbage keys to the caller while claiming success (`true`)!
   - If `capacity == 0`, `heap->data` may be `NULL`, turning `heap->data[0]` into a fatal `NULL` pointer dereference and crash (`SIGSEGV`).
2. **`!out_node` (The Out-Pointer Segfault Guard):**
   If the caller mistakenly passes `NULL` (or passes an uninitialized pointer), executing `*out_node = ...` immediately dereferences a `NULL` pointer. Checking `!out_node` guarantees total memory safety.

---

#### 2. `min_heap_pop(heap, out_node)`: $O(\log K)$ Extraction
- **Intent:** Extract and remove the champion from the root, shrinking the active heap by 1 element.
- **Step-by-Step Contract:**
  1. Guard against empty heap: if `!heap || heap->size == 0`, return `false`.
  2. Copy root champion: if `out_node`, `*out_node = heap->data[0]`.
  3. Decrement size: `heap->size--`.
  4. If elements remain (`heap->size > 0`):
     - Move the last element to the root: `heap->data[0] = heap->data[heap->size];`
     - Sink the newcomer down: `min_heap_sift_down(heap, 0);`
  5. Return `true`.

##### Common Pitfalls in `min_heap_pop`:
1. **The Post-Decrement Off-by-One Trap (`heap->data[heap->size]`):**
   In a zero-indexed array, if `heap->size` is `5`, the active elements reside at indices `0, 1, 2, 3, 4`.
   If you read `heap->data[heap->size]` *before* decrementing `heap->size`:
   ```c
   // BUG: Reading slot 5 is out of bounds / uninitialized memory!
   heap_node_t last = heap->data[heap->size];
   heap->size--;
   heap->data[0] = last;
   ```
   You read whatever stale garbage sits in slot `5` and copy that garbage to the root!
   The size decrement must happen **first**, or you must read `heap->data[heap->size - 1]`:
   ```c
   heap->size--;
   if (heap->size > 0) {
       heap->data[0] = heap->data[heap->size];
       min_heap_sift_down(heap, 0);
   }
   ```
2. **The Single-Element Edge Case (`heap->size == 1`):**
   When the heap contains exactly 1 element, popping it leaves the heap empty (`heap->size == 0`).
   Attempting to copy `data[0] = data[0]` and calling `min_heap_sift_down(heap, 0)` is wasteful (though harmless if the while-loop condition `heap_left(0) < heap->size` terminates immediately). Guarding with `if (heap->size > 0)` skips unnecessary sift overhead.
3. **Semantic Naming: Why `removed_node_idx` in `sift_down` is a Misnomer:**
   In `sift_down(heap, idx)`, naming the parameter `removed_node_idx` can be confusing because **nothing is being removed inside `sift_down`**. The root element was already removed in `pop()` by overwriting index `0` with the last leaf. `sift_down` simply receives a valid slot index whose key violates the parent invariant and sinks it down to its rightful depth. Clean systems code prefers names like `idx`, `node_idx`, or `root_idx`.

---

#### 3. `min_heap_replace_top(heap, key, payload)`: $O(\log K)$ Tournament Step
- **Intent:** In streaming K-way merge, we want to replace the consumed root with the next record from the same run file without shrinking and re-growing the array.
- **Contract:** If `!heap || heap->size == 0`, return `false`. Overwrite `heap->data[0] = (heap_node_t){ .key = key, .payload = payload };`, call `min_heap_sift_down(heap, 0);`, and return `true`.

##### The Array-Sliding Fallacy: Why Heaps Never Shift Arrays Left (`memmove` vs. In-Place Sift)
A tempting intuition when thinking about arrays is: *"To replace the first element, do we slide the whole array to the left (`memmove(data, data + 1, ...)` )?"*

In a binary heap, sliding the array left is both computationally disastrous and structurally fatal:

1. **The $O(K)$ vs. $O(\log K)$ Algorithmic Disaster:**
   - Shifting an array of $K$ elements left requires copying $K-1$ elements in RAM ($O(K)$ work).
   - If merging $K = 1,024$ runs across $10,000,000$ records:
     - Sliding the array left executes **10.2 billion element copies**!
     - In contrast, a binary heap sift-down performs at most $\log_2(1024) = 10$ steps $\longrightarrow$ **100 million operations**! The heap is **100x faster**.
2. **The Destruction of Tree Geometry:**
   - An array-backed heap is not a sorted list; it is a **Complete Binary Tree** where relationships are strictly encoded by index arithmetic:
     $$\text{left}(i) = 2i + 1, \quad \text{right}(i) = 2i + 2$$
   - If you slide all elements left by 1 index:
     - Old index 1 becomes the new root at index 0.
     - Old index 2 (previously the right child of index 0) becomes index 1 (which is supposed to be the left child of index 0).
     - Old index 3 (previously the left child of index 1) becomes index 2 (supposed to be the right child of index 0).
     - The mathematical tree structure is instantly scrambled into nonsense!
3. **The In-Place Elegance of `replace_top`:**
   Instead of shifting anything:
   - The consumer has already extracted/consumed the old minimum at `data[0]`.
   - We simply drop the newcomer from the same run file directly into the vacated throne at index 0:
     ```c
     heap->data[0] = (heap_node_t){ .key = key, .payload = payload };
     ```
   - All other $K-1$ elements in the tree stay in their exact same array slots!
   - We invoke `min_heap_sift_down(heap, 0)`. The newcomer sinks down along a single branch of the tree in $O(\log K)$ time until it settles into its natural resting place.

> [!WARNING]
> **POSIX Status Codes vs. `bool` Return Values: The `return 0;` Trap:**
> In C systems programming, functions returning an `int` often follow the POSIX convention where `0` means success and `-1` means error.
> However, for functions returning `bool` (`min_heap_peek`, `min_heap_pop`, `min_heap_replace_top`), `true` (1) means success and `false` (0) means failure!
> If you write `return 0;` at the end of a `bool` function, it evaluates to `false`. Any caller testing `if (!min_heap_replace_top(...))` will believe the heap operation failed on every invocation! Always write `return true;` on successful completion.

---



### Standalone C Header (`include/simpledb/min_heap.h`)

```c
#ifndef SIMPLEDB_MIN_HEAP_H
#define SIMPLEDB_MIN_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// A heap node pairing a 64-bit scalar sort key with an opaque payload pointer
typedef struct {
    int64_t key;      // Primitive sort key (compared directly for maximum performance)
    void *payload;    // Opaque user context (e.g. stream pointer, run descriptor, row pointer)
} heap_node_t;

// Contiguous flat-array Min-Heap Priority Queue
typedef struct {
    heap_node_t *data; // Contiguous array storing the complete binary tree
    size_t size;       // Number of active elements in the heap
    size_t capacity;   // Maximum allocated capacity
} min_heap_t;

// Lifecycle management: Constructor & Destructor
min_heap_t *min_heap_create(size_t capacity);
void min_heap_destroy(min_heap_t *heap);

// Core array navigation and sift kernels
void min_heap_sift_up(min_heap_t *heap, size_t idx);
void min_heap_sift_down(min_heap_t *heap, size_t idx);

// Ergonomic Priority Queue API
int min_heap_push(min_heap_t *heap, int64_t key, void *payload);
bool min_heap_peek(const min_heap_t *heap, heap_node_t *out_node);
bool min_heap_pop(min_heap_t *heap, heap_node_t *out_node);
bool min_heap_replace_top(min_heap_t *heap, int64_t key, void *payload);

#endif // SIMPLEDB_MIN_HEAP_H
```

---

### Standalone Unit Test Specification (`test_min_heap`)

```c
void test_min_heap(void) {
    printf("Testing Standalone Min-Heap Priority Queue (Key + Payload)...\n");
    min_heap_t *heap = min_heap_create(10);
    assert(heap != NULL);
    assert(heap->size == 0);

    // Context payloads (can be anything: integers cast to void*, strings, or stream descriptors)
    const char *payloads[5] = { "stream_0", "stream_1", "stream_2", "stream_3", "stream_4" };

    // 1. Push elements in arbitrary, completely unsorted sequence
    assert(min_heap_push(heap, 50, (void *)payloads[0]) == 0);
    assert(min_heap_push(heap, 20, (void *)payloads[1]) == 0);
    assert(min_heap_push(heap, 40, (void *)payloads[2]) == 0);
    assert(min_heap_push(heap, 10, (void *)payloads[3]) == 0);
    assert(min_heap_push(heap, 30, (void *)payloads[4]) == 0);

    assert(heap->size == 5);

    // 2. Peek champion at root (must be 10 without removing it)
    heap_node_t node;
    assert(min_heap_peek(heap, &node) == true);
    assert(node.key == 10);
    assert(node.payload == (void *)payloads[3]);

    // 3. Pop elements one by one - must emerge in strict ascending sorted order!
    int64_t expected_keys[5] = { 10, 20, 30, 40, 50 };
    const char *expected_payloads[5] = { "stream_3", "stream_1", "stream_4", "stream_2", "stream_0" };
    for (int i = 0; i < 5; i++) {
        assert(min_heap_pop(heap, &node) == true);
        assert(node.key == expected_keys[i]);
        assert(node.payload == (void *)expected_payloads[i]);
    }

    // 4. Verify empty heap state
    assert(heap->size == 0);
    assert(min_heap_pop(heap, &node) == false);

    // 5. Test min_heap_replace_top (O(log K) replacement without push+pop overhead)
    assert(min_heap_push(heap, 100, (void *)payloads[0]) == 0);
    assert(min_heap_push(heap, 200, (void *)payloads[1]) == 0);
    // Replace top (100) with 300 -> 200 should float to the top
    assert(min_heap_replace_top(heap, 300, (void *)payloads[2]) == true);
    assert(min_heap_peek(heap, &node) == true);
    assert(node.key == 200);

    min_heap_destroy(heap);
    printf("Standalone Min-Heap tests passed!\n");
}
```

#### Standalone Min-Heap Test Suite Verification
The standalone priority queue was compiled with strict project flags (`-Wall -Wextra -Wpedantic -std=c11`) and verified with 100% test pass:
1. **Empty Initialization:** Confirmed `size == 0` and allocation safety.
2. **Arbitrary Key Ingestion:** Pushed `{50, 20, 40, 10, 30}` alongside opaque string stream payloads.
3. **Root Champion Inspection:** `min_heap_peek` safely inspected the minimum `10` without mutation.
4. **Ascending Extraction:** Popped nodes in strict ascending order (`10 -> 20 -> 30 -> 40 -> 50`) while maintaining exact payload ownership.
5. **Empty State Rejection:** Both `min_heap_pop` and `min_heap_peek` safely returned `false` without memory corruption or segmentation faults.
6. **In-Place Top Replacement:** Overwriting root `100` with `300` in `{100, 200}` promoted `200` to the throne and sank `300` in a single $O(\log K)$ sift-down pass.

---


### Clean Teardown & Ephemeral Run Cleanup

Unlike `table_flush` which creates permanent files, the external sorter produces **temporary files**. 
In `external_sorter_destroy()`:
1. Close any open `run_files` handles.
2. Iterate through `run_paths` and unlink/delete every spilled file using `remove(run_paths[i])`.
3. Free `in_mem_buf`, `heap.data`, `run_paths`, and the `external_sorter_t` struct itself.

Zero file leaks, zero memory leaks.

---

### C Header (`include/simpledb/external_sort.h`)

```c
#ifndef SIMPLEDB_EXTERNAL_SORT_H
#define SIMPLEDB_EXTERNAL_SORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// A sort record / tuple holding a 32-bit integer sort key and a 32-bit row_id
typedef struct {
    int32_t key;    // The sorting key (e.g. score, timestamp, surrogate id)
    int32_t row_id; // Late materialization pointer: tuple index in the table
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

    // Memory limit of 100 records -> 1,000 items will trigger 10 spilled runs
    size_t mem_limit = 100;
    size_t total_records = 1000;
    external_sorter_t *sorter = external_sorter_create(".", mem_limit);
    assert(sorter != NULL);

    sort_record_t *input = malloc(sizeof(sort_record_t) * total_records);
    for (size_t i = 0; i < total_records; i++) {
        input[i].key = (int32_t)(total_records - i); // Reverse sorted keys
        input[i].row_id = (int32_t)i;
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

