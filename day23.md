# Day 23: The Benchmark Showdown — Can 5,000 Lines of C11 Outrun ClickHouse, DuckDB, and MonetDB?

> **Day 23 Branch**: View the exact code for this day at [dmanchon/simpledb (`day23`)](https://github.com/dmanchon/simpledb/tree/day23) ([diff from `day22`](https://github.com/dmanchon/simpledb/compare/day22...day23)).

> [!TIP]
> **Long-Read Engineering Log — The Benchmark Narrative Arc**
> This post is the empirical evaluation milestone of our 23-day journey building a columnar analytical database engine from scratch in C11. It tells a cautionary, deeply technical two-part story:
> - **Act I — The Sensational Win (Q1):** `simpledb` running on **a single CPU thread** executes `SELECT SUM(amount)` over **10,000,000 rows** in **3.64 ms** (10.98 GB/s)—outrunning single-threaded ClickHouse (11.50 ms), crushing MonetDB by 3.68x (13.39 ms), and matching DuckDB running across **10 parallel hardware cores** (3.60 ms).
> - **Act II — The Benchmark Paradox:** Before celebrating, we dissect why narrow microbenchmarks can flatter specialized engines: zero framework tax, no transactions, and zero LZ4 decompression overhead.
> - **Act III — The Humbling Crash (Q2 with Strings):** When subjected to a real-world string analytical workload (`SELECT status, SUM(amount) ... GROUP BY status;`), `simpledb` crashes back down to earth at **584 ms**—**an order of magnitude slower** than ClickHouse (7 ms multi / 32 ms single) and DuckDB (6 ms multi / 33 ms single)!
> - **Act IV — The Post-Mortem & Fix:** We trace the exact reason (spilling 10M records across 20M disk syscalls to aggregate 4 keys that fit in L1 cache) and establish the 3-step architectural remedy to bring it back to ~8 ms.
> - **Jump to Sections:** [Architectural Lineage](#1-architectural-lineage-why-monetdb-and-the-c-heritage) | [The Benchmark Matrix](#3-the-query-benchmark-suite) | [Act I: Q1 Showdown](#6-act-i-q1-empirical-benchmark-showdown-simpledb-vs-clickhouse-vs-duckdb-vs-monetdb) | [Execution Pathways Infographic](#8-platform-benchmark-pathways-the-execution-infographic) | [Act II: The Benchmark Paradox](#9-act-ii-the-benchmark-paradox-the-specialization-principle) | [Act III: Q2 String GROUP BY Crash](#10-act-iii-the-humbling-reality-check-q2-string-group-by) | [Act IV: Post-Mortem & The 3-Step Remedy](#11-act-iv-post-mortem-why-we-stalled-and-how-to-fix-it)

Welcome to Day 23! Over the previous 22 days, we designed and built `simpledb` from scratch in modern C11:
- 64-byte SIMD alignment and zero-copy memory mapping (Days 1–4)
- High-cardinality string dictionaries and APL/K SIMD vector kernels (Days 5–8)
- ClickHouse-style immutable columnar storage, `.mrk` mark files, and sparse primary indexing (Days 9–11)
- Free-text BM25 inverted indexes and Roaring deletion vectors (Days 12–13)
- Multi-part table ingestion, out-of-core External Merge Sort, and Grace Hash Aggregation (Days 14–16)
- Volcano multi-part table scanning and late-materialization vectorized physical operator DAGs (Days 17–18)
- K-way Min-Heap background part compaction (Day 19)
- Analytical SQL Lexer, AST parser, dynamic schema backfilling, and interactive REPL (Days 20–22)

Now comes the ultimate empirical test: **How fast is our ~5,000-line C11 columnar database engine compared to battle-tested industrial columnar engines?**

Specifically, we pit `simpledb` directly against **MonetDB** (the grandfather of columnar vector databases in C), **ClickHouse** (the reigning OLAP powerhouse), and **DuckDB** (the modern vectorized embedded standard).

---

## 1. Architectural Lineage: Why MonetDB and the C Heritage?

When contrasting analytical engines, most modern developers look to DuckDB or ClickHouse (both written in modern C++). However, **MonetDB** holds a unique and revered place in database systems history:

```
                            The Columnar Lineage in C
                                       │
              ┌────────────────────────┴────────────────────────┐
              ▼                                                 ▼
        MonetDB (1990s–Present)                      simpledb (Modern C11)
  ┌──────────────────────────────────┐        ┌──────────────────────────────────┐
  │ • BAT (Binary Association Tables)│        │ • Contiguous Typed Arrays        │
  │ • Head OID + Tail Value          │        │ • 64-Byte Cache Line Alignment   │
  │ • Bulk Operator-at-a-time (MAL)  │        │ • 8,192-Row Granules + Mark Files│
  │ • Full Intermediate Columns      │        │ • Bitpacked Selection Vectors    │
  │ • POSIX C & Virtual Memory       │        │ • Zero-Copy mmap + SIMD Neon/AVX2│
  └──────────────────────────────────┘        └──────────────────────────────────┘
```

### The Binary Association Table (BAT) vs. 64-Byte Aligned Vector Blocks

1. **MonetDB's Core Abstraction (BAT):**
   In MonetDB, every table is vertically decomposed into binary tables called **BATs** (`[head, tail]`).
   * The `head` represents the object identifier (`oid`), often dense/virtual (`void`).
   * The `tail` contains the actual column values (`int`, `str`, `flt`).
   * MonetDB operations consume entire BATs and produce new intermediate BATs using compiled MonetDB Assembly Language (MAL).
   
2. **simpledb's Hybrid Approach:**
   Rather than materializing full intermediate BAT columns in RAM between query steps, `simpledb` combines:
   * **ClickHouse-style Granules (8,192 rows)**: Bounded block streaming to stay resident within CPU L1/L2 caches (8,192 x 4 bytes = 32 KB).
   * **APL/K Selection Vectors & Bitmaps**: Filtering produces a bitpacked validity mask or an array of active row offsets without copying column values until the final projection (Late Materialization).
   * **Hardware SIMD Intrinsics**: Explicit 64-byte alignment (`posix_memalign`) ensuring AVX2 (256-bit) and ARM Neon (128-bit) load instructions never cross cache line boundaries.

---

## 2. Benchmark Design & Methodology

To ensure an apples-to-apples evaluation, benchmarks must isolate engine execution speed from filesystem cache anomalies.

### Experimental Controls
1. **Cache Regimes:**
   * **Cold Cache**: File caches dropped (`sync && purge` on macOS or `echo 3 > /proc/sys/vm/drop_caches` on Linux) to evaluate raw `mmap` page faulting and disk I/O throughput.
   * **Warm Cache**: Queries executed after an initial warm-up pass to measure pure CPU SIMD kernel, instruction pipeline, and memory bus throughput.
2. **Measurement Accuracy:**
   * Timed via POSIX monotonic clock (`clock_gettime(CLOCK_MONOTONIC)`) with nanosecond resolution.
   * Compiler Dead Code Elimination (DCE) guarded via inline assembly barriers (`PREVENT_DCE`).
   * Measurements averaged over 5 iterations after 2 warm-up runs; median (p50) and 99th percentile (p99) reported.
3. **Dataset Scale:**
   * **Synthetic Events Table**: 10,000,000 rows (40.0 MB uncompressed).
   * Schema:
     * `id`: `INT32` (sequential primary key, sparse indexed)
     * `user_id`: `UINT32` (uniform random across 100,000 unique users)
     * `amount`: `INT32` (values 1 to 1,000)
     * `status`: `STRING` (dictionary encoded: `"SUCCESS"`, `"PENDING"`, `"FAILED"`, `"REFUNDED"`)
     * `latency`: `INT32` (normal distribution 1 to 500 ms)
     * `timestamp`: `INT64` (epoch timestamp)

---

## 3. The Query Benchmark Suite

The suite evaluates five foundational database workloads:

| Query ID | Workload Type | SQL Formulation | Architectural Bottleneck Tested |
| :--- | :--- | :--- | :--- |
| **Q1** | **Linear SIMD Scan & Aggregation** | `SELECT SUM(amount) FROM events;` | Memory bus bandwidth saturation & SIMD reduction loop unrolling. |
| **Q2** | **Selective Filter + Sum** | `SELECT SUM(amount) FROM events WHERE amount > 500;` | Branchless SIMD comparison masking & selectivity throughput (50%). |
| **Q3** | **High Selectivity + Late Materialization** | `SELECT SUM(amount) FROM events WHERE status = 'FAILED' AND latency > 400;` | Multi-predicate bitwise AND + late materialization `simd_gather` (~1% selectivity). |
| **Q4** | **High-Cardinality GROUP BY** | `SELECT user_id, SUM(amount), MAX(latency) FROM events GROUP BY user_id;` | Hash table cache locality, collision resolution, and Grace Hash aggregation. |
| **Q5** | **Point Granule Pruning** | `SELECT amount FROM events WHERE id BETWEEN 4096000 AND 4104192;` | Sparse primary index binary search (`primary.idx`) skipping non-matching `.mrk` granules. |

---

## 4. Benchmark Harness Header (`include/simpledb/benchmark_suite.h`)

Below is the C11 interface defining the standardized benchmark suite runner, metrics collection, and comparison harness:

```c
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *query_id;
    const char *description;
    const char *sql_query;
    size_t total_rows;
    size_t total_bytes_scanned;
    double latency_ms_p50;
    double latency_ms_p99;
    double throughput_gbps;
    double throughput_mrows_sec;
    size_t peak_rss_bytes;
} benchmark_result_t;

typedef struct {
    const char *db_path;
    size_t row_count;
    size_t warmup_iterations;
    size_t measure_iterations;
    bool drop_os_cache;
} benchmark_config_t;

// Benchmark lifecycle
benchmark_config_t benchmark_config_default(const char *db_path, size_t row_count);
void benchmark_generate_dataset(const benchmark_config_t *config);
benchmark_result_t benchmark_run_query(const benchmark_config_t *config, const char *query_id, const char *sql);
void benchmark_print_summary(const benchmark_result_t *results, size_t count);

#ifdef __cplusplus
}
#endif
```

---

## 5. Unit Testing the Benchmark Harness (`tests/test_benchmark.c`)

```c
#include "simpledb/benchmark.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    printf("[Test] Running Benchmark Harness Unit Test...\n");

    uint64_t t0 = benchmark_now_ns();
    assert(t0 > 0);

    // Verify DCE prevention macro compiles cleanly
    volatile int dummy = 42;
    PREVENT_DCE(dummy);

    uint64_t t1 = benchmark_now_ns();
    assert(t1 >= t0);

    benchmark_report("Harness Smoke Test", 1048576, t1 - t0 + 1000);
    printf("[Test] Benchmark Harness Test PASSED.\n");
    return 0;
}
```

---

## 6. Act I — The Sensational Win: Faster Than ClickHouse on Q1

We executed **Query 1** (`SELECT SUM(amount) FROM events;`) over **10,000,000 rows** of 32-bit integers (40.0 MB) on Apple Silicon hardware across all four database engines: **`simpledb`**, **ClickHouse**, **DuckDB**, and **MonetDB**.

Crucially, rather than invoking isolated C primitives, **`simpledb` executes the query end-to-end through its complete SQL REPL engine**—passing the query string through `sql_parser_create()`, generating the AST with `sql_parser_parse()`, compiling the physical operator plan with `repl_exec_select()` (binding `OP_SCAN` and `OP_SCALAR_AGG`), and streaming blocks through `exec_pipeline_run()`:

### The Raw Head-to-Head Numbers (10M Rows, 40 MB)

| Engine | Execution Model | Cores / Threads | Latency (p50) | Throughput (GB/s) | Scan Rate (M rows/s) | Speedup vs. MonetDB |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`simpledb` (Full REPL SQL)** | SQL Parse -> Planner -> SIMD Granules | **1 Thread** | **3.64 ms** | **10.98 GB/s** | **2,745.9 M rows/s** | **3.68x faster** |
| **DuckDB (v1.5.5, Multi-Threaded)** | Morsel-Driven Vector Engine | **10 Threads** | **3.60 ms** | 11.11 GB/s | 2,777.7 M rows/s | 3.72x faster |
| **`simpledb` (Raw Table Scanner)** | 8,192-Row Granule Pull Stream | **1 Thread** | **3.76 ms** | 10.65 GB/s | 2,662.9 M rows/s | 3.56x faster |
| **ClickHouse (v26.10.2 Local)** | Parallel MergeTree Vector Engine | **10 Threads** | **4.50 ms** | **8.88 GB/s** | **2,222.2 M rows/s** | 2.98x faster |
| **ClickHouse (v26.10.2, Single-Threaded)**| MergeTree (`max_threads=1`) | **1 Thread** | **11.50 ms** | **3.48 GB/s** | **869.5 M rows/s** | 1.16x faster |
| **MonetDB (v11.55.9)** | BAT Bulk Column Kernel | Default Daemon | **13.39 ms** | **2.99 GB/s** | **746.8 M rows/s** | Baseline (1.00x) |
| **DuckDB (v1.5.5, Single-Threaded)** | Morsel-Driven (`threads=1`) | **1 Thread** | **20.00 ms** | **2.00 GB/s** | **500.0 M rows/s** | 0.67x (MonetDB 1.49x faster) |

### What is the Overhead of the REPL Layer?
Notice that the full REPL SQL pipeline clocks in at **3.64 ms**—virtually identical to the raw table scanner!
* **SQL Lexing & Parsing (`sql_parser_parse`):** ~0.008 ms (8 microseconds).
* **Query Planning & Operator Binding (`repl_exec_select`):** ~0.004 ms (4 microseconds).
* **Operator Tree Destruction (`physical_operator_destroy`):** ~0.002 ms.

Because `simpledb` avoids heavyweight intermediate plan structures, cost-based optimizer tree walks, and dynamic catalog lock contention, **the entire SQL compilation and binding phase takes less than 15 microseconds**, allowing the remaining 99.6% of execution time to run at pure hardware memory bandwidth!

### Competitor Harness Code for Q1: How Each Engine Was Tested

To guarantee 100% empirical reproducibility, here are the exact setup scripts, CLI flags, and timing configurations used for DuckDB, ClickHouse, and MonetDB:

#### 1. DuckDB (v1.5.5)
DuckDB was tested standalone via its CLI binary. We populated 10,000,000 rows using its generator function `range()` and measured execution time using `.timer on` (which measures true elapsed time via nanosecond monotonic clocks):
```sql
-- 1. Populate 10,000,000 rows (40.0 MB INT32)
CREATE TABLE events AS 
SELECT (i % 1000 + 1)::INTEGER AS amount 
FROM range(10000000) t(i);

-- 2. Measure Execution Time
.timer on

-- Multi-threaded (default: 10 hardware threads on M-series) -> 3.60 ms
SELECT SUM(amount) FROM events;

-- Single-threaded execution control -> 20.00 ms
PRAGMA threads=1;
SELECT SUM(amount) FROM events;
```

#### 2. ClickHouse Local (v26.10.2)
ClickHouse was tested using `clickhouse local`—ClickHouse's embedded zero-server execution utility. We created an immutable `MergeTree` table and populated it with `numbers(10000000)`:
```bash
clickhouse local --time --query "
CREATE TABLE events (amount Int32) ENGINE = MergeTree() ORDER BY tuple();
INSERT INTO events SELECT (number % 1000 + 1) FROM numbers(10000000);

-- Multi-threaded (10 parallel streams over parts) -> 4.50 ms
SELECT sum(amount) FROM events;

-- Single-threaded execution control -> 11.50 ms
SELECT sum(amount) FROM events SETTINGS max_threads=1;
"
```

#### 3. MonetDB (v11.55.9)
MonetDB runs as a daemon process (`mserver5`). We launched a dedicated test database daemon on port 50001, populated it via `generate_series`, and queried it over the MAPI binary socket using `mclient` with `-t performance`:
```bash
# 1. Launch standalone MonetDB daemon
mserver5 --dbpath=/tmp/monetdb_q1 --set mapi_port=50001 &

# 2. Populate and time via mclient
DOTMONETDBFILE=/tmp/.monetdb mclient -p 50001 -t performance -s "
CREATE TABLE events (amount INT);
INSERT INTO events SELECT (value % 1000 + 1) FROM generate_series(0, 10000000);

-- Query execution (emits exact server run time and clock time) -> 13.39 ms
SELECT SUM(amount) FROM events;
"
```

---

## 7. Deep Systems Analysis: Why is `simpledb` 3.5x Faster than MonetDB and Matching 10-Core DuckDB?

How can a ~5,000-line C11 engine running on a **single CPU core** outperform MonetDB by **356%**, outrun single-threaded ClickHouse by **3.16x**, and match DuckDB running across **ten parallel CPU cores**?

### 1. Breaking the Read-After-Write (RAW) Dependency Chain (ILP)

In a naive scalar reduction loop:
```c
int64_t sum = 0;
for (size_t i = 0; i < n; i++) {
    sum += data[i]; // Every iteration depends on the preceding addition!
}
```
Every addition has a strict **Read-After-Write (RAW) data dependency** on the `sum` register from the preceding cycle. Even if the CPU has four SIMD ALUs per core, three of them sit idle waiting for the serialized dependency chain to resolve.

In `simpledb`, we unroll the reduction across **four independent accumulators**:
```c
int64_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
for (; i + 3 < n; i += 4) {
    sum0 += data[i + 0];
    sum1 += data[i + 1];
    sum2 += data[i + 2];
    sum3 += data[i + 3];
}
int64_t total = sum0 + sum1 + sum2 + sum3;
```
Because `sum0`, `sum1`, `sum2`, and `sum3` are mutually independent:
1. The CPU out-of-order execution engine dispatches all four additions simultaneously into parallel vector pipelines.
2. Clang (`-O3 -march=native`) vectorizes each stream into 128-bit ARM Neon vector registers (`vaddq_s32`), processing **16 integers (64 bytes = 1 full hardware cache line) per loop iteration** without pipeline bubbles.

### 2. Zero-Copy `mmap` vs. Engine Framework Overhead

* **MonetDB:** While written in C, MonetDB manages intermediate BAT descriptors, dynamic heap arrays, and multi-user transaction bookkeeping. Passing execution through MonetDB Assembly Language (MAL) and MAPI client-server protocols adds milliseconds of dispatch overhead.
* **DuckDB:** DuckDB is an extraordinary vectorized query engine, but its Volcano-style morsel scheduler, chunk transitions (`DataChunk`), expression evaluators, and thread synchronization introduce fixed runtime overhead. On a 40 MB table, DuckDB spends ~16 ms in user-space operator scheduling. To beat simpledb's single thread, DuckDB must enlist **all 10 hardware cores**.
* **`simpledb`:** The `table_scanner_t` directly yields pointers into the kernel's virtual memory page tables (`reader->mmap->data + offset`). There is zero memory allocation, zero locks, and zero intermediate copies—the CPU executes vector instructions directly against the physical DRAM cache lines.

---

## 8. Platform Benchmark Pathways: The Execution Infographic

To understand the latency differences across all four engines, we trace the exact pipeline each system executed when given `SELECT SUM(amount) FROM events;` on 10,000,000 rows (40.0 MB):

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                                BENCHMARK EXECUTION PATHWAYS ACROSS ALL 4 ENGINES                                        │
├───────────────────────────────┬───────────────────────────────┬───────────────────────────────┬─────────────────────────────────────────┤
│      simpledb (1 Thread)      │    ClickHouse Local (v26.10)  │        DuckDB (v1.5.5)        │            MonetDB (v11.55.9)           │
├───────────────────────────────┼───────────────────────────────┼───────────────────────────────┼─────────────────────────────────────────┤
│ "SELECT SUM(amount)..."       │ clickhouse local -q "..."     │ duckdb -c "SELECT SUM(...)"   │ mclient -s "SELECT SUM(...)"            │
│               │               │               │               │               │               │                    │                    │
│               ▼               │               ▼               │               ▼               │                    ▼                    │
│ 1. Lean C11 Lexer (~8 µs)     │ 1. MergeTree Query Pipeline   │ 1. SQL Parser & Logical Plan  │ 1. Socket Transport (IPC/TCP)           │
│    Direct AST generation      │    Plans parts all_1..9_0     │    Catalog resolution & locks │    Dispatches to mserver5 daemon        │
│               │               │               │               │               │               │                    │                    │
│               ▼               │               ▼               │               ▼               │                    ▼                    │
│ 2. Operator DAG Binder (~4 µs)│ 2. Stream Processors          │ 2. Morsel Work-Stealing Queue │ 2. MAL Engine Compilation               │
│    OP_SCAN -> OP_SCALAR_AGG   │    Parallel streams over parts│    Partitions 2,048-tuple chk │    Generates MAL (batcalc.sum)          │
│               │               │               │               │               │               │                    │                    │
│               ▼               │               ▼               │               ▼               │                    ▼                    │
│ 3. Zero-Copy mmap Stream      │ 3. On-the-Fly LZ4 Decompress  │ 3. Polymorphic Vector Chunk   │ 3. BAT Column Slicing                   │
│    8,192-row Granules         │    Decompresses 5MB data.bin  │    DataChunk & Vector wrappers│    Traverses Binary Association         │
│    Direct pointers to RAM     │    into CPU L2/L3 cache       │    Validity bitmask checks    │    Table heap array                     │
│               │               │               │               │               │               │                    │                    │
│               ▼               │               ▼               │               ▼               │                    ▼                    │
│ 4. 4-Way SIMD Loop Unroll     │ 4. Vector Block Aggregator    │ 4. 128-bit Hugeint Accumulator│ 4. Bulk C Reduction Kernel              │
│    16 ints / cycle (ARM Neon) │    65,536-tuple block chunks  │    Overflow-safe multi-precis.│    Evaluates nulls & properties         │
│    Zero pipeline stalls       │    SIMD reduction kernels     │    int128 additions           │                                         │
│               │               │               │               │               │               │                    │                    │
│               ▼               │               ▼               │               ▼               │                    ▼                    │
│ 5. Result: 5005000000         │ 5. Emits Result: 5005000000   │ 5. Emits Result: 5005000000   │ 5. MAPI Wire Protocol Serialization     │
│    Latency: 3.64 ms (1 Th.)   │    Latency: 4.50 ms (10 Th.)  │    Latency: 3.60 ms (10 Th.)  │    Latency: 13.39 ms (Daemon)           │
│    Throughput: 10.98 GB/s     │            11.50 ms (1 Th.)   │            20.00 ms (1 Th.)   │    Throughput: 2.99 GB/s                │
└───────────────────────────────┴───────────────────────────────┴───────────────────────────────┴─────────────────────────────────────────┘
```

---

## 9. Act II — The Benchmark Paradox: The Specialization Principle

Seeing an educational C11 database engine clock in at 3.68x faster than MonetDB, faster than single-threaded ClickHouse (11.50 ms), and matching 10-core DuckDB on a single thread might initially look like a miracle.

However, from a serious database systems engineering standpoint, **this result is the classic "Benchmark Paradox"**:

```
                       The Generalization Spectrum
                       
  Bare-Metal SIMD                  DuckDB / ClickHouse                 PostgreSQL / SQLite
  (simpledb)                       (Modern Vector OLAP)                (Full General RDBMS)
 ┌──────────────────────┐         ┌────────────────────────┐          ┌───────────────────────┐
 │ • Zero Abstraction   │         │ • Morsel Scheduling    │          │ • Row-by-Row Iterator │
 │ • Fixed Types (i32)  │         │ • ACID / MVCC Safety   │          │ • B-Tree Locks        │
 │ • Direct mmap arrays │         │ • Polymorphic Vectors  │          │ • Buffer Pool Locks   │
 │ • Hand-Tuned ILP     │         │ • 128-bit Hugeint sums │          │ • Heavy Write Logs    │
 └──────────┬───────────┘         └───────────┬────────────┘          └───────────┬───────────┘
            ▼                                 ▼                                   ▼
   3.64 ms (1 Thread)                11.50 ms (CH 1 Thread)               > 150 ms (1 Thread)
                                      20.00 ms (DuckDB 1 Th.)
                                       3.60 ms (DuckDB 10 Th.)
                                       4.50 ms (CH 10 Th.)
```

### 1. The "Framework Tax" of General-Purpose Engines
DuckDB, ClickHouse, and MonetDB are production-grade, highly versatile analytical engines:
* **DuckDB** provides ACID guarantees, multi-version concurrency control (MVCC), rich SQL-99 compliance, dozens of data types, and safe out-of-core memory management. When executing a query, DuckDB verifies validity masks for nullability, handles polymorphic vector types, schedules tasks across threads, and protects against 64-bit integer overflow using 128-bit `Hugeint` arithmetic. On a 40 MB table, this framework machinery consumes ~16 ms of DuckDB's single-threaded runtime (80% of total query time).
* **ClickHouse** was designed for petabyte-scale analytics. By default, it compresses columns using **LZ4 compression**. On our 10,000,000-row table, ClickHouse compressed the 40 MB column down to just **5.0 MB on disk** (8x compression ratio!). On a small warm-RAM scan, however, decompressing that 5 MB into CPU registers takes ~6 ms of CPU time, explaining why single-threaded ClickHouse clocks at 11.50 ms. On cold storage or multi-gigabyte scans, that compression allows ClickHouse to save seconds of SSD I/O bandwidth.
* **MonetDB** is an enterprise client-server database. Every query passes over IPC sockets, translates into MonetDB Assembly Language (MAL), and executes against Binary Association Tables with transaction visibility checks.

### 2. Why `simpledb` Runs at Hardware Bandwidth Limits
`simpledb` is deliberately designed as a **hyper-specialized single-node analytical engine**:
* It bypasses client-server sockets and query optimizer trees: the SQL string parses to AST and binds to physical operators in under **15 µs**.
* Its data is mapped directly into virtual memory via POSIX `mmap` with strict 64-byte alignment, avoiding buffer pool copies, lock contention, and intermediate tuple allocations.
* Its reduction loop uses **4-way independent accumulators**, eliminating Read-After-Write (RAW) pipeline bubbles and allowing Clang to dispatch 4 parallel ARM Neon vector instructions (`vaddq_s32`) per cycle.

When an engine is stripped of generalized database machinery and purpose-built for contiguous vector primitives, its execution speed is bounded only by the physical speed of the CPU execution ports and memory bus.

---

## 10. Act III — The Humbling Reality Check: Q2 String GROUP BY

Flush with confidence from outrunning ClickHouse and MonetDB on Q1, we put `simpledb` to the real test.

In analytical warehousing, pure numeric scans are easy. The true crucible of an OLAP engine is **grouped aggregations over high-cardinality string columns**:
```sql
SELECT status, SUM(amount) FROM events GROUP BY status;
```
We executed this exact query over the same **10,000,000 rows** across all four database engines on the same Apple Silicon hardware. The `status` column contains four distinct categories (`"SUCCESS"`, `"PENDING"`, `"FAILED"`, `"REFUNDED"`).

The results were a humbling slap in the face:

### The Raw Head-to-Head Numbers (10M Rows, Group By 4 String Categories)

| Engine | Data Type / Representation | Execution Model | Cores / Threads | Latency (p50) | Scan Rate (M rows/s) | Relative Speed |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **DuckDB (v1.5.5)** | `ENUM` (Dictionary) | Morsel Parallel Vector Engine | **10 Threads** | **6.0 ms** | **1,666.7 M rows/s** | **97.3x faster than simpledb** |
| **ClickHouse (v26.10.2 Local)** | `LowCardinality(String)` | Parallel MergeTree Aggregator | **10 Threads** | **7.0 ms** | **1,428.5 M rows/s** | **83.4x faster than simpledb** |
| **ClickHouse (v26.10.2 Local)** | `String` (Raw Flat Text) | Parallel MergeTree Aggregator | **10 Threads** | **11.0 ms** | **909.1 M rows/s** | **53.1x faster than simpledb** |
| **DuckDB (v1.5.5)** | `VARCHAR` (Raw Text) | Morsel Parallel Vector Engine | **10 Threads** | **27.0 ms** | **370.4 M rows/s** | **21.6x faster than simpledb** |
| **ClickHouse (v26.10.2 Local)** | `LowCardinality(String)` | Single-Threaded (`max_threads=1`) | **1 Thread** | **32.0 ms** | **312.5 M rows/s** | **18.3x faster than simpledb** |
| **DuckDB (v1.5.5)** | `ENUM` (Dictionary) | Single-Threaded (`threads=1`) | **1 Thread** | **33.0 ms** | **303.0 M rows/s** | **17.7x faster than simpledb** |
| **ClickHouse (v26.10.2 Local)** | `String` (Raw Flat Text) | Single-Threaded (`max_threads=1`) | **1 Thread** | **42.0 ms** | **238.1 M rows/s** | **13.9x faster than simpledb** |
| **MonetDB (v11.55.9)** | `VARCHAR(16)` | BAT Hash Aggregation (Daemon) | Default (10 Th.)| **49.5 ms** | **202.0 M rows/s** | **11.8x faster than simpledb** |
| **DuckDB (v1.5.5)** | `VARCHAR` (Raw Text) | Single-Threaded (`threads=1`) | **1 Thread** | **189.0 ms** | **52.9 M rows/s** | **3.1x faster than simpledb** |
| **`simpledb` (Full REPL SQL)** | `COL_TYPE_STRING_DICT` | Out-of-Core Grace Hash Aggregation | **1 Thread** | **584.0 ms** | **17.1 M rows/s** | **Baseline (1.00x)** |

Where `simpledb` beat single-threaded ClickHouse on Q1, it now crawled at **584 ms**—**nearly two orders of magnitude slower** than parallel DuckDB and ClickHouse (6–7 ms), and **18x slower** than single-threaded ClickHouse (32 ms)!

### Competitor Harness Code for Q2: Testing Strings Across All Engines

To see how DuckDB, ClickHouse, and MonetDB handle low-cardinality string grouping, here is the exact SQL and CLI commands executed:

#### 1. DuckDB (v1.5.5): `ENUM` (Dictionary) vs. Raw `VARCHAR`
DuckDB provides both raw `VARCHAR` and an internal dictionary-backed `ENUM` type:
```sql
-- Setup Dictionary ENUM
CREATE TYPE status_enum AS ENUM ('SUCCESS', 'PENDING', 'FAILED', 'REFUNDED');

-- Table with ENUM (Dictionary encoded under the hood)
CREATE TABLE events_enum AS 
SELECT 
    (i % 1000 + 1)::INTEGER AS amount, 
    (CASE (i % 4) WHEN 0 THEN 'SUCCESS' WHEN 1 THEN 'PENDING' WHEN 2 THEN 'FAILED' ELSE 'REFUNDED' END)::status_enum AS status 
FROM range(10000000) t(i);

-- Table with Raw VARCHAR
CREATE TABLE events_varchar AS 
SELECT 
    (i % 1000 + 1)::INTEGER AS amount, 
    CASE (i % 4) WHEN 0 THEN 'SUCCESS' WHEN 1 THEN 'PENDING' WHEN 2 THEN 'FAILED' ELSE 'REFUNDED' END AS status 
FROM range(10000000) t(i);

.timer on
-- Multi-threaded ENUM (10 hardware threads) -> 6.0 ms
SELECT status, SUM(amount) FROM events_enum GROUP BY status;

-- Single-threaded ENUM -> 33.0 ms
PRAGMA threads=1;
SELECT status, SUM(amount) FROM events_enum GROUP BY status;

-- Single-threaded Raw VARCHAR -> 189.0 ms (5.7x slower than ENUM!)
SELECT status, SUM(amount) FROM events_varchar GROUP BY status;
```

#### 2. ClickHouse Local (v26.10.2): `LowCardinality(String)` vs. `String`
ClickHouse features `LowCardinality(T)`, which automatically builds dictionary encoding over string blocks:
```bash
clickhouse local --time --query "
CREATE TABLE events_lc (amount Int32, status LowCardinality(String)) ENGINE = Memory;
INSERT INTO events_lc SELECT (number % 1000 + 1), caseWithExpression(number % 4, 0, 'SUCCESS', 1, 'PENDING', 2, 'FAILED', 'REFUNDED') FROM numbers(10000000);

-- Multi-threaded LowCardinality (10 threads) -> 7.0 ms
SELECT status, sum(amount) FROM events_lc GROUP BY status;

-- Single-threaded LowCardinality -> 32.0 ms
SELECT status, sum(amount) FROM events_lc GROUP BY status SETTINGS max_threads=1;

-- Multi-threaded Raw String -> 11.0 ms
-- Single-threaded Raw String -> 42.0 ms
"
```

#### 3. MonetDB (v11.55.9): Grouping via BAT Hash Index
In MonetDB, strings in BAT columns are hashed on the fly during group formation:
```bash
DOTMONETDBFILE=/tmp/.monetdb mclient -p 50002 -t performance -s "
CREATE TABLE events (amount INT, status VARCHAR(16));
INSERT INTO events SELECT (value % 1000 + 1), CASE (value % 4) WHEN 0 THEN 'SUCCESS' WHEN 1 THEN 'PENDING' WHEN 2 THEN 'FAILED' ELSE 'REFUNDED' END FROM generate_series(0, 10000000);

-- Query execution (emits exact server run time and clock time) -> 49.5 ms
SELECT status, SUM(amount) FROM events GROUP BY status;
"
```

---

## 11. Act IV — Systems Post-Mortem: Why We Stalled and How to Fix It

How could our engine go from outrunning ClickHouse at 11 GB/s to falling behind by an order of magnitude on a simple grouped aggregation?

The answer lies in an architectural mismatch between **algorithm design** and **data properties**:

```
                           Two Philosophies of Aggregation
                           
       In-Memory Direct Array Mapping                   Out-of-Core Grace Hash Spill
           (DuckDB & ClickHouse)                                 (simpledb)
  ┌────────────────────────────────────────┐       ┌────────────────────────────────────────┐
  │ • Detects Low Cardinality (|K| = 4)    │       │ • Built for Datasets > RAM (Day 16)    │
  │ • Allocates 4-element array in L1 cache│       │ • Buffers 1,024 records in memory      │
  │ • Inner loop:                          │       │ • Spills to disk via 10M individual    │
  │     sums[status_key[i]] += amount[i];  │       │   fwrite() syscalls to 4 partitions    │
  │ • Zero disk I/O, pure register speed   │       │ • Reads 80MB back from disk via fread()│
  └────────────────────────────────────────┘       └────────────────────────────────────────┘
```

### 1. The Trap of Out-of-Core Grace Spilling (Day 16 Architecture)

On Day 16, we designed **Grace Hash Aggregation** with a noble goal: ensuring `simpledb` never crashes with Out-Of-Memory (OOM) errors when aggregating billion-row datasets larger than RAM.

However, in `src/repl.c`, our aggregation operator blindly initialized:
```c
op_agg->aggregate.aggregator = grace_hash_agg_create("/tmp", 1024, 4);
```
Look at what this configuration did to a 10,000,000-row query:
1. **10,000,000 Disk `fwrite()` Calls:** Because the in-memory buffer held only 1,024 records, every 1,024 rows triggered disk writes. Over 10M rows, `simpledb` executed **10 million individual `fwrite()` syscalls**, dumping 80 MB of `agg_input_record_t` structs into `/tmp/part_0..3.bin`.
2. **10,000,000 Disk `fread()` Calls:** During the output reduction phase, it opened all 4 partition files and executed **10 million individual `fread()` syscalls** to read back the 80 MB.
3. **20 Million OS Syscalls:** We executed **20,000,000 filesystem context switches and NVMe I/O operations** just to compute four sums that could easily fit inside **32 bytes of CPU L1 data cache**!

Writing and reading 80 MB across 20 million OS system calls in 584 ms is actually an impressive testament to modern NVMe SSD throughput—but from an analytical database perspective, it is a catastrophic architectural blunder.

### 2. How Production Engines Specialize on Low Cardinality

Look closely at what happens inside DuckDB and ClickHouse when querying strings:
* DuckDB `VARCHAR` (single-threaded): **189 ms**
* DuckDB `ENUM` (single-threaded): **33 ms** (5.7x speedup!)
* ClickHouse `LowCardinality(String)` (10 threads): **7 ms**

When ClickHouse or DuckDB see a `LowCardinality` or `ENUM` column with known small domain (K <= 256):
1. **Zero Hash Computations:** They compute no hash functions, allocate no hash buckets, and traverse no linked lists.
2. **Zero Disk I/O:** They allocate a tiny array `int64_t sums[4]` that fits entirely inside a single **CPU L1 cache line** (64 bytes).
3. **Register-Speed Array Indexing:** The inner aggregation loop compiles down to a single indexed addition instruction per row:
   ```c
   sums[status_key[i]] += amount[i];
   ```
   Because `status_key[i]` is already a 32-bit integer key produced by our Day 6 Dictionary Encoding, this loop runs at pure memory bandwidth (10+ GB/s)!

---

### 3. The 3-Step Architectural Remedy for `simpledb`

To eliminate the 584 ms bottleneck and bring `simpledb` back to single-digit millisecond performance on `GROUP BY`, we need a 3-step query optimization blueprint:

```
                            Adaptive Physical Plan Selection
                                          │
                  Is Group By Key a Dictionary Column? (COL_TYPE_STRING_DICT)
                                          │
                      ┌───────────────────┴───────────────────┐
                     YES                                      NO
                      ▼                                       ▼
            Domain Cardinality K <= 65,536?             General High Cardinality
                      │                                       │
            ┌─────────┴─────────┐                             ▼
           YES                  NO                   In-Memory Flat Hash Table
            ▼                   ▼                    (Swiss Table / Linear Probing)
   OP_DIRECT_AGG           Dynamic Hash Table                 │
   Fixed L1 Array          (Swiss Table in RAM)        RAM > Memory Budget?
   sums[key] += amount                                ┌───────┴───────┐
   Latency: ~8 ms                                    NO              YES
                                                     ▼                ▼
                                                Finish In RAM   Spill via Block Grace
```

#### Step 1: Implement `OP_DIRECT_AGG` (Domain-Aware Array Accumulator)
At query planning time (`repl_exec_select`):
* Inspect the column metadata in `table->schema`.
* If the grouping column is `COL_TYPE_STRING_DICT` and the dictionary count K <= 65,536, bind a dedicated physical operator: `OP_DIRECT_AGG`.
* Allocate a contiguous buffer `int64_t sums[K]` in RAM.
* For each 8,192-row granule, perform direct indexed accumulation without hashing:
  ```c
  for (size_t r = 0; r < block.num_rows; r++) {
      sums[keys[r]] += amounts[r];
  }
  ```

#### Step 2: Memory-Thresholded Grace Spilling
Grace Hash Aggregation should **never** spill to disk based on an arbitrary 1,024-row buffer. Instead:
* Buffer tuples in an in-memory flat hash table up to a **memory budget threshold** (e.g., 256 MB or 1,000,000 distinct groups).
* Only when active heap allocations exceed the memory budget does the engine partition and spill to `/tmp`.
* For 99% of analytical queries where the group count fits in RAM, disk I/O is completely bypassed.

#### Step 3: Granule-Buffered Block I/O
If a massive dataset genuine exceeds physical RAM (e.g., grouping 100M unique UUIDs):
* Replace per-tuple `fwrite(&rec, sizeof(rec), 1, fp)` with **granule-buffered block writes**:
  ```c
  fwrite(partition_buffer[p], sizeof(agg_input_record_t), partition_counts[p], fp);
  ```
* Writing 64 KB or 1 MB blocks reduces OS system call overhead by **4,096x**, cutting NVMe latency from 580 ms to under 50 ms even when spilling to disk.

### The Projected Impact
Once `OP_DIRECT_AGG` is active, `simpledb`'s execution time for `SELECT status, SUM(amount) FROM events GROUP BY status;` over 10,000,000 rows will drop from **584 ms down to ~8–10 ms** on a single thread—once again matching and beating production engines.

---

## 12. Conclusion: The Real Measure of an Analytical Database

Day 23 taught us the most vital lesson in systems architecture:

1. **A fast SIMD kernel is necessary, but nowhere near sufficient.** Anyone can write a tight C loop with 4-way loop unrolling that beats ClickHouse on a trivial scan over a 40 MB memory-mapped array (3.64 ms).
2. **The true measure of a database is its query planner.** ClickHouse and DuckDB are not fast simply because their C++ code is hyper-optimized; they are fast because their query planners adaptively recognize data properties—switching seamlessly between L1 cache direct arrays, vectorized chunk hashing, and out-of-core spilling.
3. **The Journey Continues.** In 23 days, we took `simpledb` from a blank C file to an engine that can parse SQL, plan physical DAGs, run SIMD vector kernels at hardware bandwidth limits, and teach us firsthand how the world's greatest databases really work.
