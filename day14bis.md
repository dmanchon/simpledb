# Day 14 Bis: Table Flush & Immutable Part Serialization

### Architectural Overview: Why Decouple Ingestion from Disk Storage?

In Day 14, we built the table ingestion layer (`table_insert_batch`), multiplexing incoming row-oriented batches into in-memory polymorphic columnar buffers (`buffer_i32_t`, `string_dict_column_t`, `raw_string_column_t`). Operating purely in RAM enables ingestion throughput exceeding millions of rows per second.

However, RAM is volatile and strictly bounded. To sustain long-running ingestion without exceeding memory quotas, analytical engines (such as ClickHouse's MergeTree engine) decouple the **write path** into two distinct stages:

1. **In-Memory Buffering (`table_insert_batch`):** Low-latency append into active RAM buffers.
2. **Immutable Part Flush (`table_flush`):** Sequential serialization of active RAM buffers to disk as a frozen, immutable **Part**. Once serialized, memory buffers are reset to accept the next batch.

```text
       Ingestion (Fast RAM)                  On-Disk Storage (Immutable)
 ┌───────────────────────────────┐           ┌───────────────────────────┐
 │ table_insert_batch()          │           │ Part 000 (.bin / .mrk)    │
 │ (Multiplexes into RAM buffers)│           ├───────────────────────────┤
 └──────────────┬────────────────┘           │ Part 001 (.bin / .mrk)    │
                │ table_flush()              ├───────────────────────────┤
                └───────────────────────────►│ Part 002 (.bin / .mrk)    │
                                             └───────────────────────────┘
```

*Why this design?* Disks—even high-performance NVMe SSDs—suffer massive performance degradation under frequent, random micro-writes. By accumulating data in RAM and executing large, **contiguous sequential block writes** during `table_flush`, we saturate maximum hardware I/O bandwidth (3–7 GB/s).

---

### When to Flush? (Triggers & Heuristics)

In a production analytical database, `table_flush` is not invoked at arbitrary times. It is triggered by deterministic heuristics:

1. **Memory Thresholds (Row Count / Byte Budget):**
   When active RAM buffers hit a predefined limit (e.g., 65,536 rows or 256 MB of RAM), the engine triggers an automatic flush to prevent Out-Of-Memory (OOM) termination.
2. **Time-Based Wall-Clock Deadlines (Flush Timeout):**
   If write volume is low, data might linger in memory for hours. A background timer (e.g., every 1 to 5 seconds) forces a flush so new records become visible to queries within bounded latency.
3. **Explicit Operational Triggers:**
   System shutdown, graceful process exit, or an explicit SQL command (such as `OPTIMIZE TABLE` or `FLUSH TABLE`) immediately serializes all pending RAM buffers.
4. **Natural Granule Boundaries:**
   Flushes are aligned with whole multiples of Granules (e.g. multiples of 8,192 rows) to keep sparse indexing files (`.mrk` and `primary.idx`) neatly partitioned without trailing fractional blocks.

---

### The Catastrophic Failure Problem & The WAL Dilemma

A crucial question in storage architecture is: **What happens if a power outage, hardware crash, or `kill -9` strikes while data is in RAM before a flush?**

If data resides exclusively in volatile RAM buffers (`buffer_i32_t`), **any un-flushed records are permanently lost**.

This brings us to the fundamental trade-off: **Should a columnar analytical database maintain a Write-Ahead Log (WAL)?**

```text
               ┌────────────────────────────────────────────────────────┐
               │              Incoming Ingestion Stream                 │
               └───────────────┬────────────────────────┬───────────────┘
                               │                        │
             Option A: With WAL                         │ Option B: Pure Analytical Batching
                               ▼                        │ (No WAL / Upstream WAL)
                 ┌───────────────────────────┐          │
                 │ Append to WAL (Disk)      │          │
                 │ (Raw sequential row log)  │          │
                 └─────────────┬─────────────┘          ▼
                               │        ┌───────────────────────────────┐
                               └───────►│ In-Memory Columnar Buffers    │
                                        └───────────────┬───────────────┘
                                                        │ table_flush()
                                                        ▼
                                        ┌───────────────────────────────┐
                                        │ Immutable Columnar Parts      │
                                        │ (.bin / .mrk / primary.idx)   │
                                        └───────────────────────────────┘
```

#### Do ClickHouse and DuckDB Use a WAL?

1. **DuckDB: Full ACID with a WAL (`.wal`)**
   - DuckDB is an embedded database designed for single-process analytical queries with ACID guarantees.
   - **Yes, DuckDB uses a WAL.** Every `INSERT`, `UPDATE`, and `DELETE` appends changes to a `.wal` file on disk before committing.
   - Periodically, DuckDB executes a `CHECKPOINT`: it flushes memory blocks to the persistent database file (`.duckdb`) and resets/truncates the `.wal` file.
   - On crash recovery, DuckDB inspects the `.wal`, replays un-checkpointed transactions, and restores consistency.

2. **ClickHouse: Historically NO WAL for Standard Inserts!**
   - ClickHouse was designed for extreme high-throughput log, metric, and event ingestion (hundreds of thousands of rows per second).
   - In ClickHouse's classical model, **there is no row WAL**. Instead:
     - The client is expected to batch rows into reasonably large chunks (e.g., 10,000 to 100,000 rows).
     - Upon receiving the batch, ClickHouse writes it **directly and synchronously as a new immutable part directory on disk**.
     - Only when the part files are safely written and sync'd does ClickHouse respond with `200 OK` to the client. Durability is achieved through immediate part creation, not a separate log!
   - **Modern Asynchronous Inserts (`async_insert=1`):**
     When clients send micro-batches (e.g. 5 rows at a time), ClickHouse buffers them in RAM. To prevent data loss, ClickHouse allows users to configure `wait_for_async_insert=1` (holding the HTTP connection open until the background batch flushes to an immutable part) or replicate batches via ClickHouse Keeper / Raft WAL across replica nodes.

#### The Core Trade-Off: Write Amplification vs. Durability

Why wouldn't every OLAP engine use a WAL?

| Feature | Row-Oriented WAL + Columnar Flush | Direct Part Flush (No WAL) |
| :--- | :--- | :--- |
| **Durability ($D$ in ACID)** | Immediate per-tuple durability. | Durability upon batch flush. |
| **Write Amplification** | **High**: Data is written **twice** to disk (once to WAL, once to columnar `.bin`). | **Optimal ($1\times$)**: Data is written exactly once straight to columnar format. |
| **Disk Bandwidth** | HALVED (saturates NVMe write bandwidth with redundant log I/O). | Maximized (100% of I/O bus is dedicated to columnar files). |
| **Micro-Batches** | Safe for single-row inserts. | Requires in-memory buffering or client-side batching. |
| **Recovery Complexity** | Requires WAL parser, log truncation, and crash replay logic. | Zero recovery logic: all completed parts on disk are already immutable and valid. |

#### The Industrial Analytical Pattern: Upstream WALs
In enterprise architectures, big data systems avoid double-writing inside the database engine by pushing the WAL responsibility **upstream**:
- Ingestion pipelines push raw events into distributed append-only commit logs like **Apache Kafka**, **Redpanda**, or **AWS Kinesis**.
- The analytical engine (`simpledb`, ClickHouse) pulls batches from Kafka into RAM and flushes immutable columnar parts.
- The engine commits its Kafka consumer offset **only after** `table_flush()` succeeds on disk.
- If the database crashes mid-flight, it simply restarts and replays from the last committed Kafka offset—giving **100% durability with 0 internal WAL overhead**!

---

### The ClickHouse Strategy: Why Every `INSERT` Is an Immediate Flush

If we follow ClickHouse's philosophy and discard the WAL, **does that mean we flush immediately on every insert?**

**YES! That is the core architecture of ClickHouse:**

```text
 Client Request                     ClickHouse Engine                     On-Disk Storage
┌────────────────────────┐         ┌─────────────────────────┐          ┌───────────────────────┐
│ INSERT INTO table      │         │ 1. Transpose to columns │          │ Create new Part:      │
│ (Batch of 50,000 rows) ├────────►│ 2. Compress blocks      ├─────────►│ part_001_001_0/       │
│                        │         │ 3. Write & fsync()      │          │ (.bin, .mrk, .idx)    │
└────────────────────────┘         └───────────┬─────────────┘          └───────────────────────┘
                                               │
                                 4. HTTP 200 OK (Durability Guaranteed!)
                                               ▼
```

1. **Synchronous Durability Without a WAL:**
   When a client sends `INSERT INTO table VALUES (...)`, ClickHouse does not put it in a loose RAM queue. It formats the batch into columnar granules and **flushes it immediately to disk as a brand-new immutable part directory**. The client connection blocks until the disk write is complete. Only when the part is safely stored does ClickHouse return `HTTP 200 OK`. 
   
   Because the data is already durable on disk as an immutable part, **no WAL is required**.

2. **The Catch: The Infamous "Too Many Parts" Error:**
   Because every `INSERT` immediately writes a new part directory to disk, what happens if a naive application inserts 1 row at a time in a tight loop of 1,000 queries per second?
   
   The engine would attempt to create 1,000 new disk directories per second! The filesystem would rapidly exhaust inode quotas, OS directory caches, and open file descriptors. 
   
   ClickHouse handles this aggressively by throwing its most famous runtime error:
   ```text
   Code: 252. DB::Exception: Too many parts in all data parts in table (300).
   Merges are processing significantly slower than inserts.
   ```

3. **Why It's Called the "MergeTree":**
   To survive immediate flushes without drowning in millions of tiny part files, ClickHouse runs background merge threads. These threads constantly take smaller adjacent parts (`part_1`, `part_2`, `part_3`), merge them into a single larger part (`part_1_3`), and delete the original smaller files.

4. **The Lesson for `simpledb`:**
   By decoupling `table_insert_batch` (RAM append) from `table_flush` (disk serialization), `simpledb` gives us both choices:
   - **Buffered Mode (Our Current Day 14 Bis Design):** Micro-inserts append to RAM buffers; when RAM is full (or when explicitly requested), we invoke `table_flush()` to create a clean part.
   - **ClickHouse Immediate Flush Mode:** Clients send large batches (e.g., 10,000+ rows), and the ingestion handler immediately invokes `table_flush()` at the end of the insert, returning success only once the part is durable on disk.

---

### The Physical Mechanics of Disk Serialization

When `table_flush(table, data_dir)` is invoked, several hardware and OS-level operations take place:

```text
 In-Memory State (RAM)                                     On-Disk Part (Permanent)
┌──────────────────────────────────────────────┐          ┌───────────────────────────────────┐
│ buffer_i32_t (Contiguous 64-byte aligned)    │ ───────► │ column.bin (Sequential Raw Bytes) │
│  [101, 102, 103, 104, 105, ...]              │ (fwrite) └───────────────────────────────────┘
├──────────────────────────────────────────────┤          ┌───────────────────────────────────┐
│ Granule Mark Tracker                         │ ───────► │ column.mrk (Byte Offsets for 8192)│
│  [Granule 0 @ 0B, Granule 1 @ 32768B]        │          └───────────────────────────────────┘
├──────────────────────────────────────────────┤          ┌───────────────────────────────────┐
│ Raw / Dictionary Strings                     │ ───────► │ column.strings (Flat String Pool) │
│  "First event\0Second event\0"               │          └───────────────────────────────────┘
└──────────────────────────────────────────────┘
```

#### 1. Zero-Branch Sequential Block I/O
Because columnar buffers store data as contiguous primitive arrays (`int32_t*`, `float*`), serialization avoids row-by-row branching. We invoke `fwrite(buf->data, sizeof(int32_t), buf->size, fp)`, allowing the OS kernel to stream consecutive 4KB disk pages straight into the OS Page Cache.

#### 2. Granule Mark Serialization (`.mrk`)
Through `column_writer_t` (built in Day 9), the flush pipeline records physical byte offsets into companion `.mrk` mark files every 8,192 rows. This preserves $\mathcal{O}(1)$ random-access entry points for future `mmap` zero-copy scans.

#### 3. String Flattening (Byte Pool + Offsets)
In-memory pointers (`char*`) cannot be stored directly to disk because pointer addresses lose validity once the process restarts. During flush:
- Raw strings are flattened into a contiguous null-terminated character pool in `column.strings`.
- 32-bit integer byte offsets into the character pool are stored sequentially in `column.bin`.

#### 4. Allocator Recycling: Why `size = 0` Instead of `free()`?

A vital systems question arises when designing ingestion pipelines:
> *"Why `buf->size = 0` or `col->keys_buf->size = 0`? Shouldn't we release memory with `free()`? Does this leak?"*

**No, it does NOT leak, and calling `free()` here would severely degrade database performance!**

To understand why, we must look at the lifecycle of high-throughput ingestion:

```text
 Ingestion Cycle:
   Batch 1 (RAM) ──► Flush to Disk ──► Batch 2 (RAM) ──► Flush to Disk ──► Batch 3 ...

 Pattern A: Free on Flush (Anti-Pattern)
   malloc() ───────► free() ─────────► malloc() ───────► free() ─────────► malloc() ...
   (High heap fragmentation, OS page faults, arena lock contention, degraded write latency)

 Pattern B: Buffer Recycling (High-Performance Systems Design)
   malloc() (Initial) ──► size = 0 ──► Ingest (Reuse) ──► size = 0 ──► Ingest (Reuse) ...
   (Zero heap allocations in steady state, warm CPU cache lines, 100% memory reuse)
```

##### 1. Size vs. Capacity in Dynamic Buffers
Every vector buffer (`buffer_i32_t`) tracks two distinct dimensions:
* `size`: How many valid logical records currently inhabit the buffer.
* `capacity`: How many total elements the heap chunk can hold before needing to grow.

When we set `col->keys_buf->size = 0`:
* We perform an $\mathcal{O}(1)$ integer assignment.
* The physical memory allocation (`col->keys_buf->data`) remains allocated and warm in RAM.
* When the next batch arrives in `table_insert_batch`, the engine writes into indices `0, 1, 2...` **without calling `malloc()` or `realloc()` at all!**

##### 2. The Cost of Heap Allocations in Production
If we called `free()` on every flush:
1. **Heap Allocator Contention:** In multithreaded engines, `malloc` and `free` must acquire locks on memory arenas (e.g., in `jemalloc` or `ptmalloc`).
2. **TLB & Soft Page Faults:** When you release memory, the OS may unmap physical pages. When you reallocate, the CPU suffers soft page faults on the first write to each 4KB virtual page.
3. **Heap Fragmentation:** Constantly allocating and releasing variable-sized buffers fragments the virtual address space.

By recycling buffers, the memory footprint quickly stabilizes at the maximum batch size, achieving **Zero-Allocation Steady-State Ingestion**.

##### 3. Primitive Flat Buffers vs. Nested Pointer Arrays
Notice the difference between the two buffer types during flush:
* **Primitive Integer Buffers (`buffer_i32_t`, `keys_buf`):**
  These contain raw numbers. There are no pointers inside `data`. Setting `size = 0` is completely leak-free because subsequent writes simply overwrite the old integers.
* **Raw String Columns (`raw_string_column_t`):**
  The `strings` array contains pointers to heap memory allocated with `strdup()`. If you only did `size = 0`, the allocated character strings would be orphaned (a true memory leak!). Therefore, you must free each individual string (`free(col->strings[r])`), but keep the outer array `col->strings` and its `capacity` intact!

##### 4. When Is Memory Actually Released?
Memory is only released when the table itself is closed or dropped: inside `table_destroy(table)`, which frees all buffers, dictionary trees, and schemas completely.

---

### Implementation Blueprint: Writing `table_flush`

To implement `table_flush`, `table_get_parts`, and `table_get_active_buffer` in `src/table.c`, follow this 4-step sequence:

```text
 table_flush(table, data_dir)
  │
  ├── 1. Validation: If table->row_count == 0, return 0 (no-op)
  │
  ├── 2. Loop Over Schema: For each column i:
  │       ├── Instantiate column_writer_create(data_dir, col.name, col.type)
  │       ├── Dispatch append:
  │       │    ├── COL_TYPE_I32 / F32: write buf->data, reset buf->size = 0
  │       │    ├── COL_TYPE_STRING_DICT: write sdc->keys_buf->data, reset size = 0
  │       │    └── COL_TYPE_STRING: write col->strings, free strings, reset size = 0
  │       └── Close writer: column_writer_close(writer)
  │
  ├── 3. Part Catalog Registration:
  │       ├── realloc(table->parts, sizeof(table_part_t*) * (table->num_parts + 1))
  │       ├── Allocate table_part_t, record data_dir and frozen row_count
  │       └── table->parts[table->num_parts++] = part
  │
  └── 4. Reset In-Memory Counter: table->row_count = 0
```

---

### Common Pitfall: The Polymorphic Buffer Trap

A tempting and concise implementation of `table_flush` looks like this:

```c
// BUGGY IMPLEMENTATION:
for (size_t i = 0; i < table->schema->count; i++) {
    column_writer_t* writer = column_writer_create(data_dir, table->schema->columns[i].name, table->schema->columns[i].type);
    column_writer_append(writer, table->column_buffers[i], table->row_count); // <- FATAL BUG!
    column_writer_close(writer);
}
```

There are three critical systems bugs in this snippet:

#### 1. Container Structs vs. Raw Data Pointers
In Day 14, we designed `table->column_buffers` as an array of `void*` pointing to **polymorphic column containers**, *not* raw value buffers:
- For `COL_TYPE_I32` / `F32`, `table->column_buffers[i]` is a `buffer_i32_t*` struct:
  ```c
  struct { int32_t* data; size_t size; size_t capacity; }
  ```
  Passing `table->column_buffers[i]` directly causes `fwrite` to write the struct's internal metadata (the memory address of `data`, the numbers `size` and `capacity`) to disk rather than the actual column values!
- For `COL_TYPE_STRING_DICT`, the integer keys reside in `((string_dict_column_t*)table->column_buffers[i])->keys_buf->data`.
- For `COL_TYPE_STRING`, the pointer array is located in `((raw_string_column_t*)table->column_buffers[i])->strings`.

Therefore, we must **switch on `col.type`** and extract the true underlying data pointer for each type before calling `column_writer_append`.

#### 2. The Un-cleared Buffer State
If `table_flush` writes to disk but fails to reset `buf->size = 0` (and free strings in `raw_string_column_t`), subsequent batch inserts will append to the old data, duplicating records on every future flush.

#### 3. Missing Part Catalog Registration
Flushing must register the new immutable part in `table->parts`. Without updating `table->parts` and resetting `table->row_count = 0`, the query layer (`table_get_parts`) will never know a new part exists on disk.

#### 4. The `NDEBUG` Assert Trap in C Build Systems
A subtle C quirk when running test suites:
If your CMake build is configured in Release mode (`-O3 -DNDEBUG`), the C preprocessor expands `assert(expr)` into `((void)0)` (a complete no-op)!
This means if you have assertions verifying buffer resets or row counts, they will be completely skipped during test execution until an explicit crash (like calling an unimplemented function that invokes `abort()`) is triggered. To guarantee assertions run during development, test executables must either be built with `CMAKE_BUILD_TYPE=Debug` or explicitly `#undef NDEBUG` before `#include <assert.h>`.

#### 5. The Dictionary Premature Free Trap
A dangerous mistake during flush is attempting to free `col->dict->strings` or set `col->dict->count = 0`:
```c
// DANGEROUS BUG IN FLUSH:
for (int r = 0; r < col->dict->count; r++) {
    free(col->dict->strings[r]);
}
col->dict->count = 0;
```
Why is this disastrous?
1. **Hash Table Corruption:** `string_dict_t` maintains an internal hash table (`dict->hash_table`). Freeing strings without clearing the hash table leaves hash buckets pointing to freed memory (use-after-free).
2. **ID Desynchronization:** If you clear the dictionary on every flush, Batch 1 maps `"US" -> 0`, and Batch 2 maps `"UK" -> 0`! Surrogate keys across parts collide and corrupt the data model.
3. **Double-Free on Shutdown:** `table_destroy()` already invokes `string_dict_free(col->dict)`. Manually freeing strings during flush causes an undefined behavior crash or double-free on shutdown.
**Rule:** Only reset the per-batch surrogate keys (`col->keys_buf->size = 0`). The dictionary vocabulary (`col->dict`) must remain untouched in RAM for the life of the table!

---

### Deep-Dive: Where Do the Strings Go? (`STRING_DICT` vs `STRING`)

A frequent point of confusion during flush implementation is how strings are stored:

#### 1. Dictionary-Encoded Strings (`COL_TYPE_STRING_DICT`)
In a dictionary-encoded column, **the strings themselves do not get appended to `column.bin`!**
- The column buffer is `string_dict_column_t { dict, keys_buf }`.
- During ingestion, strings like `"US"`, `"ES"`, `"US"` were deduplicated in memory into integer keys: `[0, 1, 0]`.
- When flushing, the data written to `<col_name>.bin` is **just the 32-bit surrogate integer keys** (`col->keys_buf->data`). 
- Because each key is a standard 4-byte `int32_t`, `column_writer_append` writes them directly as raw 4-byte integers:
  ```c
  column_writer_append(writer, col->keys_buf->data, col->keys_buf->size);
  col->keys_buf->size = 0; // Reset keys count for next batch
  ```
- The dictionary (`col->dict`) remains alive in RAM to continue encoding incoming batches with consistent ID mappings.

#### 2. Raw Variable-Length Strings (`COL_TYPE_STRING`)
In uncompressed raw string columns:
- The column buffer is `raw_string_column_t { strings, size, capacity }`.
- `col->strings` is a pointer array (`char**`).
- When you pass `col->strings` to `column_writer_append(writer, (const void*)col->strings, col->size)`, the Day 9 `column_writer` handles the two-file layout automatically:
  1. It writes string byte offsets into `col_name.bin`.
  2. It writes the null-terminated characters into `col_name.strings`.
- Because each string was dynamically allocated with `strdup()` on ingestion, `table_flush` must free each individual string before resetting `size = 0`:
  ```c
  for (size_t r = 0; r < col->size; r++) {
      free(col->strings[r]);
  }
  col->size = 0;
  ```

---

### The Dictionary Persistence Paradox: How Do We Decode Strings Later?

A critical question arises: *If we only flush integer keys to `country.bin`, how does the database tell apart new elements, and how do we reconstruct the original strings later?*

There are two separate operational contexts:

#### 1. In-Flight Ingestion (The Live Table in RAM)
While the database is running:
- `sdc->dict` (allocated on Day 6) remains permanently alive in RAM inside `table->column_buffers[i]`.
- When **Batch 1** arrives with `["US", "ES", "US"]`:
  - It inserts `"US"` -> gets ID `0`.
  - It inserts `"ES"` -> gets ID `1`.
  - It flushes `[0, 1, 0]` to disk and resets `keys_buf->size = 0`.
- When **Batch 2** arrives with `["FR", "US", "DE"]`:
  - It queries the *existing* `sdc->dict`.
  - `"FR"` is not found -> inserted as new ID `2`.
  - `"US"` IS found -> reuses ID `0`!
  - `"DE"` is not found -> inserted as new ID `3`.
  - It flushes `[2, 0, 3]` to the next part.
- Because `sdc->dict` is retained across flushes in memory, **new elements are always distinguished from existing elements** via the hash table!

#### 2. On-Disk Persistence (How ClickHouse, Parquet & `simpledb` Decode on Read)
What happens if the database process shuts down or we want to read the column back from disk?
To decode integer IDs like `[0, 1, 0, 2]` back into `"US"`, `"ES"`, `"FR"`, analytical engines use one of two architectures:

```text
  Local Part Dictionary (ClickHouse / Parquet)         Global Shared Dictionary
 ┌───────────────────────────────────────────┐        ┌───────────────────────────────────┐
 │ Part 000:                                 │        │ Table-Level Shared Catalog:       │
 │   country.bin   -> [0, 1, 0]              │        │   shared_country.dict             │
 │   country.dict  -> ["US", "ES"]           │        │   -> ["US", "ES", "FR", "DE"]     │
 ├───────────────────────────────────────────┤        ├───────────────────────────────────┤
 │ Part 001:                                 │        │ Part 000: country.bin -> [0, 1, 0]│
 │   country.bin   -> [0, 1, 2]              │        │ Part 001: country.bin -> [2, 0, 3]│
 │   country.dict  -> ["FR", "US", "DE"]     │        │                                   │
 └───────────────────────────────────────────┘        └───────────────────────────────────┘
```

- **Local Part Dictionaries (ClickHouse `LowCardinality` / Apache Parquet):**
  Each immutable part directory writes its own small companion dictionary file (`country.dict`). Part 1 maps `0 -> "US"`. Part 2 might map `0 -> "FR"`. This isolates parts so they can be merged or deleted independently without rewriting a central catalog!
- **Global Dictionary (Current `simpledb` Memory Model):**
  The table maintains one shared `string_dict_t` across all parts. When serializing the full table catalog to disk, the dictionary array (`dict->strings`) is written to `country.dict`.

#### 3. The Late Materialization Superpower
Notice why storing only keys in `country.bin` is such a massive performance win:
When executing `SELECT * FROM table WHERE country = 'US'`:
1. The engine checks the dictionary once: `'US' -> ID 0`.
2. It scans `country.bin` using hardware SIMD vector equality (`simd_eq_i32(keys, 0)`).
3. It filters millions of rows **without reading or comparing a single string from disk!**
4. Only at the very last step (output display) does it look up `dict->strings[0]` to print `"US"`.

#### 4. The Node Loss Disaster: What If We Lose the Node?

The user raised a critical systems question:
> *"If we don't store the strings on disk, how can we reconstruct the dictionary later on new batches if we lost the node?"*

The answer is brutal and unambiguous: **You CANNOT.**

If a server suffers a hardware crash, kernel panic, or node failure, and the dictionary only lived in volatile RAM (`sdc->dict`), **the mapping is lost forever**. The on-disk `country.bin` file containing `[0, 1, 0, 2]` becomes completely orphaned and undecipherable gibberish—there is no physical way to deduce that `0` meant `"US"` and `1` meant `"ES"` from the integers alone!

#### How Production Engines (ClickHouse, Parquet, DuckDB) Solve This

Production analytical systems **never** write raw dictionary keys without persisting the dictionary mapping to durable storage:

1. **ClickHouse (`LowCardinality(String)`):**
   When a part is flushed, ClickHouse writes **two separate files** per column into the part directory:
   - `country.bin`: The compressed array of integer surrogate keys (`[0, 1, 0, 2]`).
   - `country.dict`: The dictionary file storing the serialized array of unique strings (`["US", "ES", "FR"]`).
   When a node reboots or crashes, ClickHouse reads `country.dict` back into memory, instantly restoring the dictionary and hash table!

2. **Apache Parquet:**
   Parquet embeds the dictionary directly inside the column chunk! The very first page of the column chunk is designated as the **Dictionary Page** containing all unique string literals. The subsequent pages are the **Data Pages** containing the bit-packed integer keys.

3. **Production Standard for `simpledb` (No Toys!):**
   - In ephemeral runtimes (like Apache Spark shuffle buffers or in-memory Arrow frames), keeping dictionaries in RAM is acceptable because failures simply recompute the stage.
   - But for a persistent database engine like `simpledb`, **Option 1 is a toy.** A real production engine **must persist the dictionary to disk** during `table_flush()`.
   - In `table_flush`, whenever `COL_TYPE_STRING_DICT` is encountered, we serialize both the integer keys to `<col_name>.bin` and the string dictionary mapping to `<col_name>.dict`:
     ```c
     case COL_TYPE_STRING_DICT: {
         // 1. Write integer keys to column.bin
         string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
         column_writer_append(writer, col->keys_buf->data, table->row_count);
         col->keys_buf->size = 0;

         // 2. Persist the dictionary mapping to column.dict (Crash Resilience)
         char dict_path[256];
         snprintf(dict_path, sizeof(dict_path), "%s/%s.dict", data_dir,
                  table->schema->columns[i].name);
         FILE *dfp = fopen(dict_path, "wb");
         if (!dfp) {
             fprintf(stderr, "[FATAL ERROR] cannot open file %s\n", dict_path);
             exit(EXIT_FAILURE);
         }
         write_string_pool(dfp, col->dict->strings, col->dict->count);
         fclose(dfp);
         break;
     }
     ```

---

### Unifying String Storage: The Common String Pool Architecture

You asked a brilliant design question:
> *"Can we commonize both string dict raw string write and full raw string column write?"*

**YES! Under the hood, both formats are fundamentally the same physical concept: a String Pool.**

Look at the architectural symmetry between the two string representations on disk:

```text
                     COL_TYPE_STRING                          COL_TYPE_STRING_DICT
           ┌───────────────────────────────────┐        ┌───────────────────────────────────┐
Integer    │ column.bin                        │        │ column.bin                        │
Stream     │ [ 0, 12, 25, 38, ... ]            │        │ [ 0, 1, 0, 2, ... ]               │
(32-bit)   │ (Byte offsets into string pool)   │        │ (Surrogate integer keys / indexes)│
           ├───────────────────────────────────┤        ├───────────────────────────────────┤
Text       │ column.strings                    │        │ column.dict                       │
Pool       │ "apple\0banana\0cherry\0"         │        │ "apple\0banana\0cherry\0"         │
           │ (Length = N rows, duplicates ok)  │        │ (Length = Unique count, deduped)  │
           └───────────────────────────────────┘        └───────────────────────────────────┘
```

Notice the deep mechanical commonality:
1. **The Primary Binary Stream (`column.bin`):**
   In both cases, `column.bin` is simply a contiguous array of 32-bit integers (`int32_t`)!
   - In `COL_TYPE_STRING`, the integer is a **physical byte offset** into the text pool.
   - In `COL_TYPE_STRING_DICT`, the integer is a **logical entry index** into the dictionary array.
2. **The Secondary Text Stream (`column.strings` / `column.dict`):**
   In both cases, the text stream is a **dense null-terminated character pool**!

#### Commonizing the Kernel (`write_string_pool`)

Because both `col->strings` (in `raw_string_column_t`) and `dict->strings` (in `string_dict_t`) are identical in-memory structures—namely, an array of C strings (`char**`)—we can unify their write logic with a single helper function:

```c
static void write_string_pool(FILE *fp, const char **strings, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fwrite(strings[i], 1, strlen(strings[i]) + 1, fp); // Writes characters + '\0'
    }
}
```

Now, the two write paths share the exact same underlying primitive:
- For `COL_TYPE_STRING`:
  As `write_string_pool` appends each string to `strings_fp`, it records `ftell()` into `bin_fp`.
- For `COL_TYPE_STRING_DICT`:
  It writes `dict->count` to `dict_fp`, then calls `write_string_pool(dict_fp, (const char**)dict->strings, dict->count)`.

Both files become standard null-delimited string pools that can be directly mapped with `mmap` with zero translation!

#### Why Distinct Extensions? (`.strings` vs. `.dict`)

You might ask: *Wait, why `snprintf(dict_path, sizeof(dict_path), "%s/%s.dict", ...)`? Shouldn't it be `.strings`?*

They must NOT be named the same because they represent two fundamentally different on-disk formats and lifecycles:

| Property | `column.strings` (`COL_TYPE_STRING`) | `column.dict` (`COL_TYPE_STRING_DICT`) |
| :--- | :--- | :--- |
| **Who Writes It?** | Handled automatically by `column_writer_append` from Day 9. | Serialized explicitly by `table_flush` using `write_string_pool`. |
| **Data Scope** | All rows in arrival order (duplicates allowed, variable length). | Deduplicated vocabulary of unique string literals only. |
| **File Header** | None (starts immediately with byte 0 of row 0's string). | 4-byte header: `int32_t count` (total entries in dictionary). |
| **Companion `.bin`** | Contains **raw byte offsets** (`0, 12, 28...`). | Contains **dense integer IDs** (`0, 1, 0, 2...`). |
| **Addressing** | Byte address in file: `pool + bin[row]`. | Array index in dictionary: `dict->strings[bin[row]]`. |

1. **`column_writer` already creates `<col>.strings` for raw strings**:
   When you initialize `column_writer_create(data_dir, "comment", COL_TYPE_STRING)`, the writer internally opens `comment.bin` AND `comment.strings`. When you pass `col->strings` to `column_writer_append()`, it writes the characters to `.strings` and the byte offsets to `.bin` automatically.
2. **`COL_TYPE_STRING_DICT` writes integer keys to `<col>.bin` and vocabulary to `<col>.dict`**:
   The `column_writer` sees `COL_TYPE_STRING_DICT` and only writes the 4-byte surrogate keys to `<col>.bin`. The dictionary vocabulary itself (`col->dict->strings`) must be persisted as `<col>.dict` so that when loading or querying the part, the engine reads `<col>.dict` to reconstruct the dictionary array!
3. **Collision Avoidance & Self-Documentation**:
   If a column was named `.strings`, how would human operators or debugging utilities know whether the companion `.bin` stores byte offsets (requiring direct seek) or dense zero-based integer keys (requiring array indexing)? By naming it `<col>.dict`, the file extension explicitly declares the addressing semantics.

#### "Isn't That Too Implicit?" Schema-Driven Resolution vs. Sniffing Files

A sharp architectural observation is:
> *"If there is a `<col>.strings`, then `<col>.bin` contains byte offsets. If there is a `<col>.dict`, then `<col>.bin` contains dictionary keys. Isn't that pretty implicit?"*

**If the engine had to sniff the filesystem to guess what each file contained (`stat()`, `access()`), it would indeed be fragile and dangerously implicit!**

In production analytical systems, file extensions are never used for heuristic guesswork. Instead, physical files are strictly governed by **Schema-Driven Metadata**:

```text
               Table Schema Metadata (The Explicit Source of Truth)
            ┌────────────────────────────────────────────────────────┐
            │  user_id: COL_TYPE_I32                                 │
            │  country: COL_TYPE_STRING_DICT                         │
            │  comment: COL_TYPE_STRING                              │
            └────────────────────────┬───────────────────────────────┘
                                     │
                 Deterministic File Contract (Zero Guesswork)
                                     ▼
   ┌─────────────────────────────────┬─────────────────────────────────┐
   │ If COL_TYPE_STRING_DICT:        │ If COL_TYPE_STRING:             │
   │  - Open country.bin (as keys)   │  - Open comment.bin (as offsets)│
   │  - Open country.dict (as vocab) │  - Open comment.strings (pool)  │
   └─────────────────────────────────┴─────────────────────────────────┘
```

1. **The Schema is the Contract:**
   When the loader (Day 17's `table_scanner_t`) opens a part directory, it does not scan the folder asking *"Gee, what files exist here?"*. It iterates over `table->schema->columns`.
   The column type in the schema explicitly dictates what files must exist and how the CPU must interpret the binary bytes in `.bin`.
2. **How ClickHouse Does It (`columns.txt`):**
   In ClickHouse, every part directory contains an explicit metadata manifest called `columns.txt`:
   ```text
   columns format version: 1
   3 columns:
   `user_id` Int32
   `country` LowCardinality(String)
   `comment` String
   ```
   ClickHouse reads `columns.txt` first. The type definition `LowCardinality(String)` is the explicit contract that tells ClickHouse to load `country.bin` as keys and `country.dict.bin` as the dictionary.
3. **How Apache Parquet Does It:**
   In Parquet, the file footer contains a Thrift schema metadata structure defining `type: INT32`, `converted_type: UTF8`, `encoding: PLAIN_DICTIONARY`. The decoder knows exactly what the binary pages represent before reading a single data byte.

In short: the different extensions (`.dict` vs `.strings`) ensure that physical files never collide on disk, but the interpretation of those files is **100% explicit, governed by the schema!**

---

### C Language Quirk: Why `char**` Does Not Implicitly Convert to `const char**`

When calling `write_string_pool(dfp, col->dict->strings, col->dict->count);`, Clang/GCC throws:
```text
warning: passing 'char **' to parameter of type 'const char **' discards qualifiers in nested pointer types
[-Wincompatible-pointer-types-discards-qualifiers]
```

At first glance, this seems bizarre: if `char*` can implicitly convert to `const char*`, why can't `char**` convert to `const char**`?

This is a classic C type-safety rule (C11 §6.3.2.3 and §6.5.16.1). If C allowed implicit conversion from `char**` to `const char**`, you could accidentally overwrite read-only memory with zero compiler warnings:

```c
const char *read_only = "immutable";
char *mutable_ptr;
char **pp = &mutable_ptr;

// If C allowed implicit conversion:
const char **cpp = pp;   // pp points to mutable_ptr
*cpp = read_only;        // mutable_ptr now points to "immutable"!
*mutable_ptr = 'X';      // CRASH! Modifying read-only memory through non-const pointer!
```

To resolve this warning cleanly in C, you have two idiomatic options:

#### Option A: Explicit Cast
```c
write_string_pool(dfp, (const char **)col->dict->strings, col->dict->count);
```

#### Option B: Match the Signature to `char **` (or `char *const *`)
Because both `col->strings` and `dict->strings` are mutable arrays of string pointers (`char**`), define the helper to accept `char**` directly:
```c
static void write_string_pool(FILE *fp, char **strings, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fwrite(strings[i], 1, strlen(strings[i]) + 1, fp);
    }
}
```
Now `write_string_pool(dfp, col->dict->strings, col->dict->count);` compiles cleanly with zero warnings and no cast required!

#### Does Casting to `const` Have Any Advantage?

**In terms of runtime performance or machine code: Absolutely NONE.**

1. **Zero Hardware Difference:**
   In C, `const` does not exist at runtime. Pointers are just 64-bit integer addresses loaded into CPU registers. The compiler generates the **exact same assembly code** whether a pointer is marked `const` or not.
2. **Compile-Time Intent Only:**
   The only purpose of `const` in a parameter list is to document developer intent: *"I promise this function will only read from this memory and will not mutate it."*
3. **Why Casting is Actually a Anti-Pattern Here:**
   Forcing an explicit cast like `(const char **)col->dict->strings` has zero benefits and introduces a real downside: **it silences the compiler**. In C, brute-force casts blind the compiler's type checker, which can mask genuine memory bugs down the road.

Therefore, **Option B (using `char **strings`) is strictly superior**: it requires no ugly casts, avoids compiler trickery, and represents the true mutable heap ownership of `dict->strings` and `col->strings`!

---

### What Completes `table_flush`? (The Final 3 Steps)

After the for-loop finishes writing all columns to disk, you cannot just `return 0`. You must finalize the table's state:

1. **Grow the Parts Array:**
   ```c
   table_part_t **new_parts = realloc(table->parts, sizeof(table_part_t *) * (table->num_parts + 1));
   if (!new_parts) return -1;
   table->parts = new_parts;
   ```
2. **Allocate and Populate `table_part_t`:**
   ```c
   table_part_t *part = malloc(sizeof(table_part_t));
   if (!part) return -1;
   strncpy(part->part_path, data_dir, sizeof(part->part_path) - 1);
   part->part_path[sizeof(part->part_path) - 1] = '\0';
   part->row_count = table->row_count;
   table->parts[table->num_parts++] = part;
   ```
3. **Reset Table Row Count and Return:**
   ```c
   table->row_count = 0;
   return 0;
   ```

---

### Part Registration and Read Path Decoupling

Flushing generates an immutable **Part** tracked in the table's part catalog:

```c
typedef struct table_part_t {
    char part_path[256];
    size_t row_count;
} table_part_t;
```

Each flush appends a `table_part_t` descriptor to `table->parts` and increments `table->num_parts`. 

To read across the engine, query engines use two decoupled metadata APIs:
1. `table_get_parts(table, &out_num_parts)`: Returns all immutable disk parts to be scanned via `column_reader_t`.
2. `table_get_active_buffer(table, &out_row_count)`: Returns live in-memory columnar buffers that have not yet been flushed to disk.

#### Who Is Using `table_get_active_buffer` Really? (The Read-Your-Own-Writes Dilemma)

You asked a brilliant question:
> *"Who is using `table_get_active_buffer` really?"*

Consider what happens in a real database without this function:

```text
 Client 1: Ingests 500 rows ──► Stored in RAM (table->column_buffers)
                                (Flush threshold is 65,536 rows, so NO flush occurs yet)
 
 Client 2: Immediately executes: SELECT count(*) FROM table;
```

If the query engine only looked at disk parts via `table_get_parts()`:
* The query would return only the on-disk count (e.g. 100,000).
* The 500 rows inserted 1 millisecond ago would be **completely invisible**!
* From Client 1's perspective, this violates basic database consistency: *"I just inserted 500 rows and got OK, why can't I see them in my query?!"*

```text
                        Incoming Query: SELECT * FROM table
                                         │
                         ┌───────────────┴───────────────┐
                         ▼                               ▼
                 On-Disk Parts                   In-Memory Buffer
              (table_get_parts)             (table_get_active_buffer)
             Part 000, Part 001...             Pending 500 rows in RAM
                         │                               │
                         └───────────────┬───────────────┘
                                         ▼
                            Day 17 Multi-Part Scanner
                          (Unified 8192-row block stream)
                                         ▼
                            Day 18 Vectorized Pipeline
```

##### The LSM / Hybrid Scanner Pattern
In systems architecture, this is the classic **MemTable + SSTable** union pattern (used in RocksDB, Cassandra, DuckDB, and ClickHouse):
1. **Historical Data:** Stored on disk in massive, immutable, compressed columnar parts.
2. **Fresh Data:** Held in volatile RAM buffers (`table->column_buffers`).
3. **The Multi-Part Table Scanner (Day 17):**
   When executing a query, Day 17's `table_scanner_t` invokes `table_get_parts()` to stream through historical disk blocks via `mmap`, and **then calls `table_get_active_buffer()` to stream through the un-flushed in-memory records**.
4. Both streams are multiplexed into standard 8,192-row execution chunks (`block_t`). 

Result: Queries always observe **100% up-to-the-millisecond consistency (Read-Your-Own-Writes)** without forcing the database to hammer the disk with tiny, inefficient micro-flushes!

---

### The Read Path Architecture: Startup Loading vs. Lazy Query Execution

A foundational architectural question is:
> *"Where is the read path? When loading the app, do we read all the parts into memory, or is this done lazily?"*

The answer is: **Data is ALWAYS loaded lazily. Loading all parts at startup would be catastrophic for an analytical database!**

Imagine a table with 1 Billion rows taking 500 GB on disk. If the database attempted to read all parts into memory during startup:
- Booting the engine would take 10+ minutes just reading disk bytes.
- The server would instantly crash with Out-Of-Memory (OOM).

Instead, OLAP systems (ClickHouse, DuckDB, `simpledb`) employ the **Eager Metadata, Lazy Data** architecture:

```text
  STARTUP PHASE (db_open)                                   QUERY EXECUTION PHASE (SELECT)
 ┌──────────────────────────────────────┐                  ┌────────────────────────────────────────┐
 │ 1. Scan data_dir for part directories│                  │ 1. 2-Tier Pruning (In-Memory)          │
 │ 2. Load sparse index (primary.idx)   │                  │    Skip 95% of granules with 0 disk I/O│
 │    into RAM (~2 MB total!)           │                  ├────────────────────────────────────────┤
 │ 3. Register table_part_t catalog     │                  │ 2. Lazy Zero-Copy mmap                 │
 └──────────────────────────────────────┘                  │    Only read matched 8192-row granules │
                                                           ├────────────────────────────────────────┤
                                                           │ 3. Union with Active RAM Buffer        │
                                                           │    (table_get_active_buffer)           │
                                                           └────────────────────────────────────────┘
```

#### 1. Startup Phase: Eager Metadata, Zero Data
When the database opens:
- The engine scans the directory (`opendir`) to discover part paths (`part_000`, `part_001`...).
- It reads **metadata only**:
  1. The sparse index (`primary.idx` from Day 10): 1 boundary key per 8,192 rows. Even for 1 Billion rows, this takes less than **2 MB of RAM** and loads in milliseconds!
  2. The row count of each part.
- **The actual data files (`.bin`, `.strings`, `.dict`) are NOT read into memory!**

#### 2. Query Execution Phase: 3-Tier Lazy Read Pipeline
When a user executes a query (e.g. `SELECT score FROM events WHERE user_id > 5000`):
1. **Tier 1: Index Pruning (0 Disk I/O):**
   The query engine runs binary search on the in-memory sparse index. If a part or granule has `[min, max]` below 5,000, it is **completely pruned**. Zero bytes are touched on disk!
2. **Tier 2: Lazy `mmap` Chunked Loading (`column_reader_t` from Day 11):**
   For the few granules that survived pruning, `column_reader_t` maps the `.bin` file with `mmap()`. Virtual memory addresses are mapped without loading physical bytes. The OS kernel only pulls 4KB physical pages into the OS Page Cache when the CPU loops through that specific granule!
3. **Tier 3: Active Buffer Union (`table_get_active_buffer`):**
   What about fresh rows that were inserted into RAM via `table_insert_batch` 2 milliseconds ago and haven't been flushed yet?
   The query engine calls `table_get_active_buffer(table, &active_rows)` to scan those un-flushed memory buffers directly, unioning them with the disk stream.

This is why `table_t` does not have a `table_read()` method: `table_t` is purely the **storage and lifecycle manager**. Query reading is delegated to the **Vectorized Execution Pipeline (Day 17)**!

#### The Curriculum Roadmap: Which Day Does What?

To see the master blueprint of how all these pieces click together across our 19-day plan:

| Milestone | Subsystem | Responsibility |
| :--- | :--- | :--- |
| **Day 3 & Day 11** | Zero-Copy Column Reader (`column_reader_t`) | Reads 8,192-row granules lazily via `mmap`, skipping unselected blocks. *(Implemented)* |
| **Day 10** | Sparse Index Pruner (`sparse_index_t`) | Evaluates query bounds against `primary.idx` in RAM without touching disk. *(Implemented)* |
| **Day 14 & 14 Bis** | Table Ingestion & Flush (`table_t`) | Manages RAM insertion buffers, triggers immutable disk flushes, and exposes metadata getters (`table_get_parts`, `table_get_active_buffer`). *(Today)* |
| **Day 15 & Day 16** | Out-of-Core Execution (`ORDER BY` & `GROUP BY`) | External Merge Sort and Partitioned Grace Hash Aggregation. |
| **Day 17** | **The Multi-Part Table Scanner (`table_scanner_t`)** | **Dedicated Data Loading Day!** Unifies pulling lazily from `column_reader_t` across all immutable disk parts, pruning via `sparse_index`, and unioning live RAM buffers into 8,192-row `block_t` streams. |
| **Day 18** | **Vectorized Physical Execution Pipeline (`exec_pipeline_t`)** | The compute engine: assembles `filter` (APL compress) $\rightarrow$ `gather` (late materialization) $\rightarrow$ `project` $\rightarrow$ `aggregate`. |
| **Day 19** | **Minimal SQL Parser & REPL CLI Shell** | The user-facing shell: `db_open()` scans disk directories on boot, executes SQL queries against the pipeline, and displays formatted results in < 50ms! |

---

### C Header (`include/simpledb/table.h`)

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

// --- Write Path Lifecycle ---
table_t *table_create(const char *name);
void table_destroy(table_t *table);
int table_insert_batch(table_t *table, const void ***columnar_values, size_t num_columns, size_t num_rows);
int table_flush(table_t *table, const char *data_dir);

// --- Read Path (Metadata Accessors) ---
table_part_t **table_get_parts(table_t *table, size_t *out_num_parts);
void **table_get_active_buffer(table_t *table, size_t *out_row_count);

#endif // SIMPLEDB_TABLE_H
```

---

### End-to-End Verification (`tests/test_db.c`)

```c
void test_table_flush_e2e(void) {
    printf("Testing Table Flush E2E (Disk Serialization & Buffer Reset)...\n");

    // 1. Create table & setup 4-column schema
    table_t *table = table_create("events_flush");
    assert(table != NULL);

    schema_registry_t *schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "score", COL_TYPE_F32);
    schema_registry_add_column(schema, "country", COL_TYPE_STRING_DICT);
    schema_registry_add_column(schema, "comment", COL_TYPE_STRING);
    table->schema = schema;

    table->column_buffers = calloc(schema->count, sizeof(void *));
    table->column_buffers[0] = buffer_i32_create(4);
    table->column_buffers[1] = buffer_i32_create(4);

    string_dict_column_t *sdc = malloc(sizeof(string_dict_column_t));
    sdc->dict = string_dict_create();
    sdc->keys_buf = buffer_i32_create(4);
    table->column_buffers[2] = sdc;

    table->column_buffers[3] = raw_string_column_create(4);

    // 2. Ingest batch of 3 rows
    int32_t user_ids[] = { 201, 202, 203 };
    float scores[] = { 10.5f, 20.0f, 30.5f };
    const char *countries[] = { "US", "UK", "US" };
    const char *comments[] = { "alpha", "beta", "gamma" };

    const void **batch[4];
    batch[0] = (const void *)user_ids;
    batch[1] = (const void *)scores;
    batch[2] = (const void *)countries;
    batch[3] = (const void *)comments;

    assert(table_insert_batch(table, (const void ***)batch, 4, 3) == 0);
    assert(table->row_count == 3);

    // 3. Flush in-memory data to disk
    assert(table_flush(table, ".") == 0);

    // 4. Assert in-memory buffer was reset
    assert(table->row_count == 0);
    buffer_i32_t *c0 = (buffer_i32_t *)table->column_buffers[0];
    assert(c0->size == 0);
    assert(c0->capacity >= 4); // Capacity retained!
    (void)c0;

    // 5. Assert part catalog registered the new immutable part
    assert(table->num_parts == 1);
    assert(table->parts[0]->row_count == 3);

    // 6. Test Read-Path Metadata APIs
    size_t active_rows = 999;
    void **active_bufs = table_get_active_buffer(table, &active_rows);
    assert(active_bufs != NULL && active_rows == 0);
    (void)active_bufs;

    size_t num_parts = 0;
    table_part_t **parts = table_get_parts(table, &num_parts);
    assert(parts != NULL && num_parts == 1);
    assert(parts[0]->row_count == 3);
    (void)parts;

    // 7. Verify serialized files exist on disk
    FILE *f_user_id = fopen("./user_id.bin", "rb");
    assert(f_user_id != NULL);
    fclose(f_user_id);

    FILE *f_country_dict = fopen("./country.dict", "rb");
    assert(f_country_dict != NULL);
    fclose(f_country_dict);

    FILE *f_comment_strings = fopen("./comment.strings", "rb");
    assert(f_comment_strings != NULL);
    fclose(f_comment_strings);

    // Cleanup generated disk files
    remove("./user_id.bin");
    remove("./score.bin");
    remove("./country.bin");
    remove("./country.dict");
    remove("./comment.bin");
    remove("./comment.strings");

    table_destroy(table);
    printf("Table Flush E2E tests passed!\n");
}
```

---

## Final Implementation (`src/table.c`)

```c
#include "simpledb/table.h"
#include "simpledb/allocator.h"
#include "simpledb/fts.h"
#include "simpledb/schema_registry.h"
#include "simpledb/storage.h"
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
    if (!table)
        return;

    if (table->column_buffers && table->schema) {
        for (size_t i = 0; i < table->schema->count; i++) {
            if (!table->column_buffers[i])
                continue;

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

    if (table->fts) {
        inverted_index_free(table->fts);
    }

    for (size_t i = 0; i < table->num_parts; i++) {
        free(table->parts[i]);
    }
    if (table->parts)
        free(table->parts);

    schema_registry_free(table->schema);

    free(table);
}

int table_insert_batch(table_t *table, const void ***columnar_values, size_t num_columns,
                       size_t num_rows) {
    if (!table || !table->schema || !table->column_buffers) {
        return -1;
    }
    if (num_columns != table->schema->count) {
        return -1;
    }
    for (size_t i = 0; i < table->schema->count; i++) {
        if (!table->column_buffers[i])
            continue;

        switch (table->schema->columns[i].type) {
        case COL_TYPE_I32: {
            buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
            buffer_i32_ensure_capacity(buf, num_rows + buf->size);
            memcpy((int32_t *)buf->data + buf->size, columnar_values[i],
                   sizeof(int32_t) * num_rows);
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
            string_dict_encode_column(col->dict, (const char **)columnar_values[i], num_rows,
                                      col->keys_buf);
            break;
        }

        case COL_TYPE_STRING: {
            raw_string_column_t *col = (raw_string_column_t *)table->column_buffers[i];
            if (col->size + num_rows > col->capacity) {
                size_t new_cap = (col->capacity == 0) ? 128 : col->capacity * 2;
                while (new_cap < col->size + num_rows) {
                    new_cap *= 2;
                }
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

static void write_string_pool(FILE *fp, char **strings, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fwrite(strings[i], 1, strlen(strings[i]) + 1, fp);
    }
}

int table_flush(table_t *table, const char *data_dir) {
    if (!table || !table->schema || !table->column_buffers || table->row_count == 0) {
        return 0; // Nothing to flush
    }

    for (size_t i = 0; i < table->schema->count; i++) {
        if (!table->column_buffers[i])
            continue;

        column_writer_t *writer = column_writer_create(data_dir, table->schema->columns[i].name,
                                                       table->schema->columns[i].type);
        switch (table->schema->columns[i].type) {
        case COL_TYPE_I32:
        case COL_TYPE_F32: {
            buffer_i32_t *buf = (buffer_i32_t *)table->column_buffers[i];
            column_writer_append(writer, buf->data, table->row_count);
            buf->size = 0;
            break;
        }
        case COL_TYPE_STRING_DICT: {
            // first write the dict keys
            string_dict_column_t *col = (string_dict_column_t *)table->column_buffers[i];
            column_writer_append(writer, col->keys_buf->data, table->row_count);
            col->keys_buf->size = 0;
            // then write the original strings
            char dict_path[256];
            snprintf(dict_path, sizeof(dict_path), "%s/%s.dict", data_dir,
                     table->schema->columns[i].name);
            FILE *dfp = fopen(dict_path, "wb");
            if (!dfp) {
                fprintf(stderr, "[FATAL ERROR] cannot open file %s\n", dict_path);
                exit(EXIT_FAILURE);
            }
            write_string_pool(dfp, col->dict->strings, col->dict->count);
            fclose(dfp);
            break;
        }

        case COL_TYPE_STRING: {
            raw_string_column_t *col = (raw_string_column_t *)table->column_buffers[i];
            column_writer_append(writer, col->strings, col->size);
            for (size_t r = 0; r < col->size; r++) {
                free(col->strings[r]);
            }
            col->size = 0;
            break;
        }
        }
        column_writer_close(writer);
    }
    // Register the new immutable part in table->parts
    table_part_t **new_parts = realloc(table->parts, sizeof(table_part_t *) * (table->num_parts + 1));
    if (!new_parts) return -1;
    table->parts = new_parts;
    table_part_t *part = malloc(sizeof(table_part_t));
    if (!part) return -1;
    strncpy(part->part_path, data_dir, sizeof(part->part_path) - 1);
    part->part_path[sizeof(part->part_path) - 1] = '\0';
    part->row_count = table->row_count;
    table->parts[table->num_parts++] = part;
    // Reset table row count
    table->row_count = 0;
    return 0;
}

table_part_t **table_get_parts(table_t *table, size_t *out_num_parts) {
    if (!table) {
        if (out_num_parts) *out_num_parts = 0;
        return NULL;
    }
    if (out_num_parts) *out_num_parts = table->num_parts;
    return table->parts;
}

void **table_get_active_buffer(table_t *table, size_t *out_row_count) {
    if (!table) {
        if (out_row_count) *out_row_count = 0;
        return NULL;
    }
    if (out_row_count) *out_row_count = table->row_count;
    return table->column_buffers;
}
```
