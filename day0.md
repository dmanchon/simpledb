
Before writing the initial lines of C code for `simpledb`, it is essential to outline the core motivations for the project, its technical ancestry, and the explicit constraints of its design.

## 1. Motivations

Most software applications treat database engines as black boxes accessed via high-level query languages. Building a columnar database kernel from scratch in plain C11 provides a clear view into lower-level systems engineering concepts.

### Hardware-Aware Memory Alignment
Modern SIMD vector registers (ARM Neon, Intel AVX2, AVX-512) require data to be aligned to hardware cache line boundaries. Aligning allocations to 64-byte boundaries prevents split cache-line fetches, enabling memory bandwidth scans of 40 to 80 GB/s.

### Zero-Copy Memory Mapping
Operating system primitives like `mmap` allow binary column files on disk to be mapped directly into user-space virtual memory addresses, bypassing kernel buffer copies and user-space memory allocations.

### Vectorized Execution Primitives
Structuring data manipulation around array language primitives (compress, gather, group) allows row-by-row iteration to be transformed into tight, branchless vector loops.

### C11 Minimalism
Many C++ database implementations rely on extensive template metaprogramming, complex class hierarchies, and virtual method dispatch. Plain C11 eliminates template bloat, ensures predictable code generation, and gives explicit control over memory layouts and pointer arithmetic.

## 2. Technical Inspirations

The architecture of `simpledb` draws upon design principles established by four major analytical systems.

```
                          simpledb Architectural Ancestry
                                         │
      ┌──────────────────┬───────────────┴───────────────┬──────────────────┐
      │                  │                               │                  │
┌───────────┐    ┌───────────────┐               ┌───────────────┐  ┌─────────────┐
│ ClickHouse│    │  APL / K / q  │               │    DuckDB     │  │Apache Arrow │
└─────┬─────┘    └───────┬───────┘               └───────┬───────┘  └──────┬──────┘
      │                  │                               │                 │
Sparse Primary      Vector Array                    Zero-Copy Slices  Zero-Copy
Index & Granules   compress/gather                 Late Materialization Export
```

### ClickHouse
ClickHouse demonstrates the efficiency of sparse primary indexing. By partitioning data into 8,192-row granules accompanied by mark files, the primary index size is reduced by a factor of 8,192, allowing indexing structures for billion-row tables to fit entirely inside L2 and L3 CPU caches.

### APL and K/q
The array programming lineage introduced by Arthur Whitney proves that data processing is fastest when formulated as bulk operations over contiguous primitive vectors (`int32_t*`) rather than object-oriented row tuples.

### DuckDB
DuckDB highlights the benefit of zero-copy typed slices and late materialization. Physical query operators act as read-only window views over underlying column buffers without duplicating data during intermediate query steps.

### Apache Arrow
Apache Arrow establishes a standardized, language-agnostic C Data Interface. Adopting compatible columnar memory layouts allows database buffers to be exposed to external runtimes like Python, Polars, and NumPy without serialization overhead.

## 3. Explicit Non-Goals

To maintain a lean implementation targeted under 2,500 lines of C11, `simpledb` intentionally excludes several traditional database subsystems:

- Commercial Production Readiness: The engine is designed as an educational reference implementation for columnar database internals rather than a general-purpose production store.
- Full ANSI SQL Compliance: The project avoids complex SQL grammar parsers, query planners, and cost-based optimizers in favor of a minimal vector REPL shell.
- Heavy Transactional Overhead: `simpledb` targets analytical workloads (OLAP) and excludes multi-version concurrency control (MVCC) or heavy ACID logging frameworks required by transactional databases.
- External C++ Dependencies: The codebase consists of pure POSIX C11, compiling without external third-party dependencies under strict compiler warning flags.
