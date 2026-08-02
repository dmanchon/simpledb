# Building a Database Engine in C11 — Day 0: Motivations & Influences

> **Series**: *Building `simpledb` — A High-Performance Columnar Engine in C11*  
> **Reading Time**: 6 minutes

---

Welcome to **Day 0** of `simpledb`!

Before writing line 1 of code, it is vital to establish **why** this project exists, the **historical & architectural engineering influences** behind it, and what we explicitly **choose NOT to build**.

---

## 1. Motivations: Why Build a Columnar Engine in Plain C11?

### A. Demystifying the "Database Black Box"
99\% of software engineers use databases purely as consumer abstractions (`SELECT * FROM table`). 

Building an analytical database kernel from scratch in **Plain C11** forces us to confront fundamental low-level mechanics:
* **Hardware Cache-Line Alignment**: Why 64-byte alignment prevents split cache-line fetches and allows ARM Neon / Intel AVX-512 vector registers to scan data at **40–80 GB/s**.
* **Zero-Copy Operating System Mapping**: How `mmap()` maps binary column files directly into CPU pointers, bypassing kernel buffer copies entirely.
* **Vectorized Execution**: How array language operations (`compress`, `gather`, `group`) transform row-by-row loops into hardware SIMD instructions.

### B. C11 Minimalism & Mechanical Sympathy
Modern C++ database engines often drown in template metaprogramming, complex class hierarchies, and virtual method dispatch (`vtable`) overhead.

By choosing **Plain C11**, we enforce:
1. **Zero Template & Binary Bloat**: Only code explicitly written is compiled into the binary.
2. **Explicit Memory Control**: Pointers, contiguous memory buffers (`buffer_i32_t`), and explicit allocation lifecycles.
3. **Direct Hardware & OS Mapping**: Transparent, predictable code that maps 1-to-1 with CPU machine assembly.

---

## 2. Standing on the Shoulders of Giants: Technical Inspirations

The design of `simpledb` draws direct inspiration from four pioneering systems:

```
                          ┌───────────────────────────────────────┐
                          │   `simpledb` Architectural Ancestry   │
                          └───────────────────┬───────────────────┘
                                              │
      ┌──────────────────┬────────────────────┴───────────────────┬──────────────────┐
      ▼                  ▼                                        ▼                  ▼
┌───────────┐    ┌───────────────┐                       ┌─────────────────┐  ┌─────────────┐
│ ClickHouse│    │  APL / K / q  │                       │ DuckDB / C-Store│  │Apache Arrow │
│ & Tinybird│    │ (Arthur W.)   │                       │  (Stonebraker)  │  │  C Data ABI │
└─────┬─────┘    └───────┬───────┘                       └────────┬────────┘  └──────┬──────┘
      │                  │                                        │                  │
      ▼                  ▼                                        ▼                  ▼
Sparse Primary      Vector Array                            Zero-Copy Slices,     Zero-Copy
Index & Granules   `compress`/`gather`                     Late Materialization  Python Export
```

### A. ClickHouse & Tinybird
* **Inspiration**: The **MergeTree** storage engine architecture, **Sparse Primary Indexing (`primary.idx`)**, **8,192-row Granule block partitioning**, and companion **Marks Files (`.mrk`)**.
* **Lesson**: Indexing blocks of rows (granules) rather than individual rows reduces index memory footprint by **8,192x**, fitting the entire primary index of 1,000,000,000 rows into **2MB of CPU L2/L3 cache**.

### B. APL & K/q (Kx Systems / Arthur Whitney)
* **Inspiration**: Array language vectorized execution primitives (`compress`, `gather`, `group`).
* **Lesson**: Data processing is fastest when structured as bulk operations over flat primitive arrays (`int32_t*`) rather than object-oriented row tuples.

### C. DuckDB & C-Store Paper (Stonebraker et al., 2005)
* **Inspiration**: Zero-copy typed slice execution (`slice_i32_t`) and late materialization (delaying row reconstruction until after filtering).
* **Lesson**: Physical query operators should never allocate memory or copy bytes—they should operate as read-only window views over columnar data.

### D. Apache Arrow
* **Inspiration**: The **Arrow C Data Interface** (`struct ArrowArray` and `struct ArrowSchema`).
* **Lesson**: Columnar memory buffers should be directly exportable to Python (Pandas/Polars/NumPy) via zero-copy pointer sharing in 0.00ms.

---

## 3. Explicit Non-Goals (What We Choose NOT to Build)

To keep `simpledb` laser-focused on hardware SIMD performance and systems learning, we establish strict **Non-Goals**:

* ❌ **NOT a Commercial Production Replacement**: We are not attempting to replace ClickHouse or DuckDB in production.
* ❌ **NO Full ANSI SQL Parser**: No multi-thousand-line SQL grammar, CTEs, or complex query optimizer. We build a lean, minimal SQL/APL REPL shell.
* ❌ **NO Heavy ACID / MVCC Transactions**: `simpledb` is designed specifically for high-throughput OLAP (Analytical Append) workloads, not OLTP banking transactions.
* ❌ **NO Heavy External C++ Dependencies**: 100% pure POSIX C11 code compiling cleanly under `-Wall -Wextra -Wpedantic -Werror`.

---

## What's Next?

* **Day 1**: [64-Byte SIMD Memory Alignment & Dynamic Column Buffers](day1.md)
* **Day 2**: [Zero-Copy Typed Slices & C11 `_Generic` Polymorphism](day2.md)
