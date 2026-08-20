# Low-Level Columnar Analytical Database (`simpledb`) Plan (C11 Edition)

## 1. Project Overview & Vision

`simpledb` is a low-level, high-performance, single-node columnar analytical database engine built from scratch in **Modern C11**. It combines concepts from:
- **APL & K/q Array Languages**: Native vector operators (compress, scan, gather/scatter, group indexing).
- **MonetDB**: BAT array decomposition, Bulk column primitives, Late Materialization.
- **ClickHouse**: Vectorized block execution, MergeTree concepts, Granule sparse indexing.
- **Honeycomb Retriever**: Dynamic schema-less columnar layout, low-cost high-cardinality string dictionaries, and zero-schema event handling.
- **Modern SOTA Systems**: Vortex/Lance direct SIMD on compressed data, Roaring Bitmap deletion vectors.
- **First-Class Free-Text Search**: Inverted postings lists, SIMD tokenization, BM25 scoring.

---

## 2. Why Plain C11 for a Database Engine?

1. **Zero Template Bloat & Zero C++ Ceremony**: No `std::allocator_traits`, no template metaprogramming, no virtual function tables (`vtable`), no name mangling.
2. **Explicit Memory Control**: Pointers, contiguous arrays, explicit lifetime management (`malloc`/`free`), and explicit cache line alignment.
3. **Direct Hardware & OS Mapping**: Direct `posix_memalign`, `mmap`, `madvise`, and SIMD intrinsics (ARM Neon / Intel AVX2) over typed array pointers (`int32_t*`, `float*`).
4. **Follows Industrial Giants**: Redis, Linux kernel, SQLite, DuckDB execution core, and MonetDB are built in C.

---

## 3. Industry Landscape: What Exists Out There vs. `simpledb` (C11)

| Engine | Storage Model | Vector / Array Engine | Query Syntax | Unique Strength | Limitations / Why `simpledb` is Different |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **kdb+ / q** (Kx Systems) | Columnar / Mapped Files | **Native K Vector Operations** | `q` (Custom Array Syntax) | Sub-millisecond time-series & array queries | Proprietary license, non-standard `q` syntax |
| **MonetDB** | BAT (Binary Association Tables) | Bulk BAT Kernels (C) | SQL | Pioneered vectorized column stores in C | Heavy legacy codebase |
| **ClickHouse** | MergeTree LSM Columnar | Granule-based Vector Loops | SQL | Extreme scale for log analytics | Heavy LZ4/Zstd CPU decompression before SIMD runs |
| **DuckDB** | Column-Group Block | Vectorized Engine (C/C++) | SQL | Embedded, rich SQL features | Lacks native APL/K array reduction operators & integrated FTS |
| **`simpledb`** | **Zero-Copy `mmap` + Deletion Vectors** | **APL & K/q SIMD Primitives (C11)** | **Hybrid SQL + APL Array Syntax** | **Ultra-lean (~2,500 LOC C11), SIMD on compressed data, native FTS** | Designed for single-node high-performance analytical workloads |

---

## 4. APL & K/q Vector Primitive Operator Mapping in C11

| Vector Operation | APL Symbol | K/q Syntax | C11 SIMD Function in `simpledb` |
| :--- | :--- | :--- | :--- |
| **Compress / Filter** | `X / Y` | `where X` or `X#Y` | `simd_compress_i32(const int32_t* src, const uint8_t* mask, int32_t* dst, size_t n)` |
| **Sum Reduction** | `+/ X` | `sum X` | `simd_sum_i32(const int32_t* src, size_t n)` |
| **Max / Min Reduction** | `⌈/ X` / `⌊/ X` | `max X` / `min X` | `simd_max_i32(const int32_t* src, size_t n)` / `simd_min_i32` |
| **Scan / Prefix Sum** | `+\ X` | `sums X` | `simd_prefix_sum_i32(const int32_t* src, int32_t* dst, size_t n)` |
| **Gather / Lookup** | `X[Y]` | `X@Y` or `X Y` | `simd_gather_i32(const int32_t* src, const uint32_t* oids, int32_t* dst, size_t n)` |
| **Group Classification** | `X ⍳ Y` | `group X` or `X=Y` | `build_group_index_i32(const int32_t* src, size_t n, group_index_t* out)` |

---

## 5. Prototyping Strategy to Keep Lines of Code (LOC) Low (~2,500 LOC C11)

| Module | Focus Area | Estimated LOC |
| :--- | :--- | :--- |
| **Phase 1** | 64-byte aligned allocators (`aligned_malloc`), `int32_vector_t`, Dynamic Schema Registry | ~250 LOC |
| **Phase 2** | High-cardinality string dictionaries, FOR + Bit-Packing SIMD kernels | ~350 LOC |
| **Phase 3** | Free-text search engine (Inverted postings, SIMD tokenization, BM25 kernel) | ~300 LOC |
| **Phase 4** | Zero-copy `mmap` storage, Header metadata, Min/Max Zone Maps | ~300 LOC |
| **Phase 5** | External Merge Sort, Grace Hash Aggregation, Bitset Deletion Vectors | ~450 LOC |
| **Phase 6** | APL / K physical operators (`seq_scan`, `compress`, `gather`, `aggregate`) | ~500 LOC |
| **Phase 7** | Minimal SQL & K/q Query Parser & CLI REPL loop | ~350 LOC |
| **Total** | **Full Database Engine in C11** | **~2,500 LOC** |

---

## 6. Detailed Step-by-Step Curriculum: Day-by-Day (C11 Edition)

---

### Module A: C11 Essentials & Memory Management (Days 1–4)

#### 🔹 Day 1: 64-Byte SIMD Memory Alignment & Allocation in C11
- **The WHY**: SIMD hardware (AVX2 / ARM Neon) operates on 128/256/512-bit registers. Unaligned addresses force split cache-line fetches.
- **Data Structures**: `aligned_malloc()`, `aligned_free()`, `buffer_i32_t`
- **Daily Code Goal**: Implement 64-byte `aligned_malloc` and a dynamic array struct `buffer_i32_t`.

#### 🔹 Day 2: C11 Structs, Type Safety & Generic Views
- **The WHY**: C11 `_Generic` macros and explicit typed struct views (`slice_i32_t`) provide type safety without template bloat.
- **Data Structures**: `slice_i32_t`, `column_type_t`
- **Daily Code Goal**: Build zero-copy slice struct `slice_i32_t` wrapping array pointers and lengths.

#### 🔹 Day 3: Systems I/O & Memory-Mapped Files (`mmap`)
- **The WHY**: `mmap` maps disk files directly into memory pointers (`const int32_t*`), bypassing buffer copy overhead.
- **Data Structures**: `mmap_file_t`
- **Daily Code Goal**: Implement `mmap_file_t` mapping binary files to 64-byte aligned read-only array slices.

#### 🔹 Day 4: Build System & Micro-Benchmarking in C
- **The WHY**: Optimization flags (`-O3 -march=native`) and high-resolution POSIX timing (`clock_gettime(CLOCK_MONOTONIC)`).
- **Data Structures**: `benchmark_runner`
- **Daily Code Goal**: Measure memory bandwidth throughput (GB/s) of aligned array scans vs unaligned scans.

---

### Module B: Columnar Data Structures & APL/K Vectors in C (Days 5–8)

#### 🔹 Day 5: Column Vectors & Selection Vectors (`Gather / Lookups`)
- **The WHY**: Late materialization avoids copying non-matching columns during filtering.
- **Data Structures**: `column_t`, `selection_vector_t`
- **Daily Code Goal**: Implement selection vector filter generating OID indices for matched rows.

#### 🔹 Day 6: High-Cardinality String Dictionaries
- **The WHY**: Converts variable-length string comparisons into 1-byte or 2-byte integer array comparisons.
- **Data Structures**: `string_dict_t`
- **Daily Code Goal**: Implement string dictionary encoding string vectors into `uint16_t` key arrays.

#### 🔹 Day 7: Null Bitmaps & Dynamic Schema Registry
- **The WHY**: Bit-packed validity masks cost 1 bit per row for nullability instead of wasting 4-8 bytes.
- **Data Structures**: `null_bitmap_t`, `schema_registry_t`
- **Daily Code Goal**: Implement dynamic JSON event parser ingesting schema-less events into columnar array buffers.

#### 🔹 Day 8: SIMD APL Vector Primitives (`Compress & Reduction`)
- **The WHY**: Compute 8–16 elements per clock cycle using ARM Neon or Intel AVX2 intrinsics.
- **Data Structures**: `simd_kernels.h`
- **Daily Code Goal**: Write SIMD filter & compress C kernel executing sub-millisecond selection over 4 million elements.

---

### Module C: On-Disk Columnar Format & Storage Engine (Days 9–12)

#### 🔹 Day 9: ClickHouse-Style On-Disk Column Layout & Marks Files (`.mrk`)
- **The WHY**: Storing thousands of individual granule files exhausts OS file handles. Writing a single column file (`column.bin`) accompanied by a **Marks File (`column.mrk`)** provides $O(1)$ byte-offset entry points into 8,192-row granules while using 1 file descriptor per column.
- **Data Structures**: `column_writer_t`, `mark_entry_t`, `part_metadata_t`
- **Daily Code Goal**: Write binary serializer creating immutable column parts with `.bin` files and companion `.mrk` mark offset files.

#### 🔹 Day 10: ClickHouse-Style Sparse Primary Index (`primary.idx`) & 2-Tier Pruner
- **The WHY**: Dense indexing costs $O(N)$ memory. Storing 1 primary key boundary per 8,192-row granule reduces index size by **$8192\times$**, fitting the entire index of 1,000,000,000 rows into **2MB of CPU L2/L3 cache**.
- **Data Structures**: `sparse_index_t`, `granule_range_t`, `part_pruner_t`
- **Daily Code Goal**: Build 2-tier query pruner executing Part-level `[min, max]` filtering and Granule-level `primary.idx` binary search skipping non-matching blocks.

#### 🔹 Day 11: The Column Reader (Zero-Copy Data Loading)
- **The WHY**: We need a pull-based iterator that takes the `bitmap_t` from the Pruner, uses `mmap` to bypass user-space copying, and yields valid 8,192-row Granules to the execution engine.
- **Data Structures**: `column_reader_t`
- **Daily Code Goal**: Build zero-copy chunked reader skipping masked-out granules.

#### 🔹 Day 12: Free-Text Inverted Block Indexing & BM25 Scoring
- **Data Structures**: `inverted_index_t`, `bm25_scorer_t`
- **Daily Code Goal**: Build inverted postings index tokenizing text columns and scoring relevance.

---

### Module D: Out-of-Core Processing & Execution Pipeline (Days 13–17)

#### 🔹 Day 13: Roaring Bitset Deletion Vectors
- **Data Structures**: `deletion_vector_t`
- **Daily Code Goal**: Implement deletion bitmasks bypassing file rewrite compactions.

#### 🔹 Day 14: External Merge Sort (Disk Spilling `ORDER BY`)
- **Data Structures**: `external_sorter_t`
- **Daily Code Goal**: Implement run generator spilling sorted runs to disk under memory limits.

#### 🔹 Day 15: Partitioned Grace Hash Aggregation (Disk Spilling `GROUP BY`)
- **Data Structures**: `grace_hash_agg_t`
- **Daily Code Goal**: Build disk-spilling hash aggregator handling high-cardinality `GROUP BY`.

#### 🔹 Day 16: APL / K Vectorized Physical Execution Pipeline
- **Data Structures**: `physical_operator_t`, `exec_pipeline_t`
- **Daily Code Goal**: Assemble pipeline executing `scan` -> `filter` -> `gather` -> `aggregate`.

#### 🔹 Day 17: Minimal SQL & K/q Query Parser & REPL CLI
- **Data Structures**: `sql_lexer_t`, `sql_parser_t`, `repl_t`
- **Daily Code Goal**: Complete the working database engine REPL shell in C11 executing queries in sub-50ms!

---

## 7. Fantasy Code Snippet: How Query Execution Looks in C11

```c
#include "simpledb/engine.h"
#include <stdio.h>

int main(void) {
    // 1. Zero-Copy mmap Column Store
    database_t* db = db_open("./events_data");

    // 2. Query Pipeline Execution in C11
    slice_dict_t status_col  = db_get_column_dict(db, "status");
    slice_i32_t  amount_col  = db_get_column_i32(db, "amount");
    slice_i32_t  latency_col = db_get_column_i32(db, "latency");
    slice_u32_t  user_col    = db_get_column_u32(db, "user_id");

    // 3. Vector Predicate Masking & FTS Index
    bitmap_t fts_mask    = fts_index_match(db->fts, "log_msg", "timeout connection");
    bitmap_t status_mask = simd_equals_dict(&status_col, "SUCCESS");
    bitmap_t amount_mask = simd_greater_than_i32(&amount_col, 100);
    
    bitmap_t filter_mask = simd_bit_and3(&status_mask, &amount_mask, &fts_mask);

    // 4. APL Compress (X / Y) -> Selection Vector of OIDs
    selection_vector_t oids = simd_compress_oids(&filter_mask);

    // 5. Late Materialization Gather (X@Y)
    buffer_u32_t filtered_users   = simd_gather_u32(&user_col, &oids);
    buffer_i32_t filtered_amounts = simd_gather_i32(&amount_col, &oids);
    buffer_i32_t filtered_latency = simd_gather_i32(&latency_col, &oids);

    // 6. K Group Classification & APL Reductions
    group_index_t group_idx = build_group_index_u32(&filtered_users);
    buffer_i64_t total_spent = simd_group_sum_i32(&filtered_amounts, &group_idx); // +/
    buffer_i32_t max_latency = simd_group_max_i32(&filtered_latency, &group_idx); // ⌈/

    // 7. Output Results
    printf("+---------+-------------+-------------+\n");
    printf("| user_id | total_spent | max_latency |\n");
    printf("+---------+-------------+-------------+\n");
    for (size_t i = 0; i < group_idx.num_groups && i < 10; ++i) {
        printf("| %-7u | %-11lld | %-9d ms |\n",
               group_idx.keys[i], total_spent.data[i], max_latency.data[i]);
    }
    printf("+---------+-------------+-------------+\n");

    db_close(db);
    return 0;
}
```

---

## 8. Productivizing `simpledb`: Python C Extension Bindings & Zero-Copy Integration

To transition `simpledb` from a C11 database engine into an enterprise-grade product used by data engineers, data scientists, and ML pipelines (similar to **DuckDB**, **PyArrow**, **Polars**, and **NumPy**), we bridge the C11 engine directly into **Python**.

---

### A. Architectural Pipeline: C11 Engine $\rightarrow$ Python API

```
┌─────────────────────────────────────────────────────────┐
│                    Python Application                   │
│   df = db.query("SELECT ...").to_pandas() / to_arrow()  │
└────────────────────────────┬────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────┐
│               Python C Extension Wrapper                │
│    (PyCapsule / CPython C-API / Python Buffer Protocol) │
└────────────────────────────┬────────────────────────────┘
                             │ Zero-Copy Shared Pointers (64-byte Aligned)
                             ▼
┌─────────────────────────────────────────────────────────┐
│              `simpledb` C11 Core Engine                 │
│   SIMD Filter / APL Vectors / mmap / 64-Byte Allocator  │
└─────────────────────────────────────────────────────────┘
```

---

### B. Core Productivization Strategies

#### 1. C Shared Library Target (`libsimpledb.dylib` / `libsimpledb.so`)
CMake produces a shared dynamic library with a clean C ABI:
```cmake
add_library(simpledb_shared SHARED src/db.c src/allocator.c src/engine.c)
set_target_properties(simpledb_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
```

#### 2. Zero-Copy Python Buffer Protocol (`Py_buffer` / Arrow C Data Interface)
Instead of serializing or copying memory, `simpledb` exports its 64-byte aligned C buffers (`buffer_i32_t`) directly to Python using the **Arrow C Data Interface** (`struct ArrowArray` and `struct ArrowSchema`):

```c
// Export 64-byte aligned C buffer to PyArrow / Polars without copying bytes!
struct ArrowArray export_buffer_to_arrow(buffer_i32_t* buf) {
    struct ArrowArray array;
    array.length = buf->size;
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 2;
    array.buffers = malloc(sizeof(void*) * 2);
    array.buffers[0] = NULL;       // Null bitmap (none)
    array.buffers[1] = buf->data;  // 64-byte aligned C int32_t array pointer!
    return array;
}
```

#### 3. Pythonic Package API (`simpledb` Python Package)
Users install via `pip install simpledb` and query using Python:

```python
import simpledb
import pyarrow as pa
import pandas as pd

# Connect to C11 columnar storage
db = simpledb.connect("./events_data")

# Execute SIMD analytical query
result = db.query("""
    SELECT user_id, sum(amount) AS total_spent, max(latency) AS max_latency
    FROM events
    WHERE status = 'SUCCESS' AND amount > 100
    GROUP BY user_id
""")

# Convert to Pandas / PyArrow / Polars with ZERO memory copying (0.00ms latency!)
arrow_table = result.to_arrow()
df = result.to_pandas()

print(df.head())
```

---

### C. Distribution & CI/CD Pipeline (`cibuildwheel` & PyPI)

| Pipeline Stage | Tool | Description |
| :--- | :--- | :--- |
| **C Compilation** | CMake 3.20+ | Compiles `-O3 -march=native` shared C library |
| **Python Bindings** | `nanobind` or CPython C-API | Lean 10KB binding layer with 0 runtime overhead |
| **Wheel Packaging** | `cibuildwheel` | Automatically builds pre-compiled `.whl` binaries for macOS ARM64 (M1/M2/M3), macOS x86_64, and Linux x86_64 |
| **PyPI Publishing** | GitHub Actions | Automatically publishes wheels on tagged git releases (`pip install simpledb`) |

---

## 9. Long-Term Roadmap: Distributed Architecture, HA, Sharding & Replication

To scale `simpledb` beyond single-node workloads to multi-terabyte / petabyte distributed analytical clusters (similar to **ClickHouse Cluster**, **Snowflake**, and **CockroachDB / YugabyteDB**), the engine evolves into a **Shared-Nothing Distributed Architecture**.

---

### A. Distributed Cluster Topology

```
                         ┌─────────────────────────────────┐
                         │   Coordinator / Router Node     │
                         │ (SQL Planner, Partition Router) │
                         └────────────────┬────────────────┘
                                          │
                  ┌───────────────────────┴───────────────────────┐
                  │ Arrow Flight RPC / Zero-Copy Network Shuffle  │
                  ▼                                               ▼
┌───────────────────────────────────┐           ┌───────────────────────────────────┐
│     Worker Node 1 (Shard A)       │           │     Worker Node 2 (Shard B)       │
│ ┌───────────────────────────────┐ │           │ ┌───────────────────────────────┐ │
│ │  Primary Replica (Raft Leader)│ │           │ │  Primary Replica (Raft Leader)│ │
│ └──────────────┬────────────────┘ │           │ └──────────────┬────────────────┘ │
│                │ Raft WAL Sync    │           │                │ Raft WAL Sync    │
│                ▼                  │           │                ▼                  │
│ ┌───────────────────────────────┐ │           │ ┌───────────────────────────────┐ │
│ │  Follower Replica (Node 3)    │ │           │ │  Follower Replica (Node 4)    │ │
│ └───────────────────────────────┘ │           │ └───────────────────────────────┘ │
└───────────────────────────────────┘           └───────────────────────────────────┘
```

---

### B. Sharding & Data Partitioning Strategies

1. **Consistent Hash Sharding (`shard_key`)**:
   * Rows are distributed across storage worker nodes using MurmurHash3 / CityHash on a shard key:
     $$\text{Node ID} = \text{hash}(\text{tenant\_id}) \pmod N$$
   * Prevents cross-node data shuffling for single-tenant / entity-scoped queries.

2. **Time-Series / Range Partitioning**:
   * Data is chunked into immutable monthly or daily partition blocks (`partition_date = 'YYYY-MM'`).
   * **Partition Pruning**: Query coordinator automatically skips entire node shards if query bounds fall outside the partition time window.

3. **Distributed Exchange Operators (`Hash Repartitioning & Broadcast Joins`)**:
   * For large distributed JOINs, intermediate SIMD selection vectors are repartitioned across nodes using an **Exchange Physical Operator (`scatter_exchange` / `gather_exchange`)**.

---

### C. High Availability (HA) & Replication Protocol

1. **Raft Consensus Protocol (`libraft` / C11 Raft Engine)**:
   * Each shard operates as a **Raft Consensus Group** consisting of 1 Primary Leader and $R - 1$ Follower Replicas (e.g. 3-way replication factor).
   * Shard topology, schema migrations, and node memberships are stored in a linearizable metadata catalog.

2. **WAL Streaming & Active-Passive Replication**:
   * Writes (`INSERT / DELETE`) are appended to a local **Write-Ahead Log (WAL)** on the Raft Leader.
   * WAL log entries are streamed asynchronously or synchronously to follower nodes before committing.

3. **Automatic Failover & Self-Healing (< 2s RTO)**:
   * If a Worker Node fails health checks (heartbeat timeout > 1000ms), the Raft quorum automatically elects a new Leader from the active followers without downtime.

---

### D. Zero-Copy Network Transport (Apache Arrow Flight & RDMA)

| Technology | Purpose | Performance Benefit |
| :--- | :--- | :--- |
| **Apache Arrow Flight** | gRPC-based IPC network transport for columnar data batches | Streams 64-byte aligned SIMD record batches directly over TCP sockets without JSON/Protobuf serialization |
| **RDMA (InfiniBand / RoCE)** | Kernel-bypass Remote Direct Memory Access | Transfers memory directly from Node A's C heap to Node B's C heap at **100 Gbps** with sub-microsecond latency |
| **Object Store Offloading (S3 / GCS)** | Cold storage tiering for historical partitions | Moves immutable column blocks to S3 Parquet / Lance storage with local SSD caching |

---

## 10. Introspection & Telemetry: Virtual System Tables (`system.*`)

To provide enterprise-grade observability, configuration tuning, and real-time operational diagnostics (similar to **ClickHouse `system.*`**, **PostgreSQL `pg_stat_*`**, and **DuckDB `duckdb_*`**), `simpledb` implements **Virtual System Tables**.

---

### A. Virtual Table Architecture (`system` Virtual Schema)

Virtual system tables do not exist on disk. When queried (e.g., `SELECT * FROM system.metrics`), the query engine invokes C internal provider callbacks (`system_table_provider_t`) that generate 64-byte aligned columnar record batches directly from live memory atomics, OS stats, and allocator metrics:

```c
typedef struct {
    const char* table_name;
    void (*populate_cb)(database_t* db, table_builder_t* builder);
} system_table_provider_t;
```

---

### B. Core Virtual System Tables

#### 1. `system.config` (Engine Configuration & Settings)
Exposes engine runtime parameters and memory limits. Supports SQL `SHOW` and `SET`:

```sql
SELECT name, value, description, is_readonly FROM system.config;
```

| name | value | description | is_readonly |
| :--- | :--- | :--- | :--- |
| `simd_alignment` | `64` | SIMD hardware alignment in bytes | `true` |
| `mmap_max_bytes` | `10737418240` | Maximum memory-mapped cache limit (10 GB) | `false` |
| `max_worker_threads` | `16` | Parallel vector execution worker threads | `false` |

#### 2. `system.metrics` (Real-Time Hardware & Engine Telemetry)
Exposes low-level C memory counters, SIMD throughput, and cache metrics:

```sql
SELECT metric_name, value, unit FROM system.metrics WHERE metric_name LIKE 'simd%';
```

| metric_name | value | unit |
| :--- | :--- | :--- |
| `memory_aligned_allocated_bytes` | `419430400` | bytes |
| `simd_throughput_gbps` | `64.2` | GB/s |
| `mmap_active_handles` | `48` | count |
| `cache_hit_ratio` | `0.984` | ratio |

#### 3. `system.queries` (Active Query Diagnostics & Profiling)
Tracks live running queries, execution duration, and row scan rates:

```sql
SELECT query_id, query_text, duration_ms, rows_scanned, memory_bytes 
FROM system.queries 
WHERE duration_ms > 100;
```

#### 4. `system.shards` & `system.raft` (Distributed Cluster Observability)
In distributed mode, tracks cluster topology, Raft consensus state, and replication lag:

```sql
SELECT shard_id, node_id, is_leader, term, log_index, replication_lag_bytes 
FROM system.raft;
```

---

### C. Prometheus & OpenTelemetry Exporter Integration

Because `system.metrics` is a standard SQL virtual table, the `simpledb` Python SDK and HTTP management port can export metrics directly in **Prometheus exposition format**:

```python
# Prometheus metrics endpoint powered by system.metrics
from prometheus_client import start_http_server, Gauge
import simpledb

db = simpledb.connect("./events_data")

# Periodically scrape system.metrics and expose to Grafana / Datadog
mem_gauge = Gauge("simpledb_aligned_memory_bytes", "Memory allocated via 64-byte aligned_malloc")
throughput_gauge = Gauge("simpledb_simd_throughput_gbps", "SIMD memory scan throughput in GB/s")

def poll_telemetry():
    metrics = db.query("SELECT metric_name, value FROM system.metrics").to_dict()
    mem_gauge.set(metrics["memory_aligned_allocated_bytes"])
    throughput_gauge.set(metrics["simd_throughput_gbps"])```

---

## 11. Future Work & Benchmarking (jsonbench.com)

While `simpledb` is designed as a foundational, educational C11 prototype over a 16-day curriculum, its architecture maps directly to the techniques used by industry-leading systems that compete on benchmarks like **[jsonbench.com](https://jsonbench.com)**.

To evolve `simpledb` from this 16-day prototype into an engine capable of processing the massive JSON analytical workloads seen in jsonbench (e.g., 500GB of deeply nested raw JSON events), the following architectural extensions would be required:

1. **Robust JSON Path Parsing (`simdjson`)**: Extending the Day 7 dynamic schema registry to handle recursive, deeply nested object paths (e.g., extracting `data.commit.collection`), rather than just flat key-value pairs.
2. **Multi-Threaded Ingestion Pipeline**: Parallelizing the JSON parsing and column encoding to saturate all available CPU cores.
3. **Advanced On-Disk Compression**: Implementing dictionary-aware block compression using Zstd or LZ4 to dramatically reduce the disk I/O bottlenecks when scaling up to 1 Billion rows.
