# `simpledb` — Low-Level Columnar Analytical Database Engine (C11 Edition)

`simpledb` is a high-performance, single-node, out-of-core columnar analytical database engine written from scratch in **Modern C11**.

---

## 1. Architectural Highlights

- **Zero Template Overhead**: Built strictly in C11 (`-std=c11`) with zero C++ template metaprogramming, vtables, or header bloat.
- **64-Byte SIMD Memory Alignment**: Raw aligned memory allocations (`aligned_malloc`) matching 64-byte hardware cache line boundaries (ARM Neon / Intel AVX2 / AVX-512).
- **ClickHouse-Style Sparse Primary Indexing**: 8,192-row Granule block partitioning with Marks Files (`.mrk`) reducing primary index size by 8,192x.
- **APL & K/q Vector Primitives**: Dynamic array buffers (`buffer_i32_t`) executing bulk array operations (`compress`, `sum`, `max`, `gather`, `group`).
- **Zero-Copy Memory-Mapped Storage**: `mmap` binary file format reading columns directly into memory pointers.
- **Vectorized SIMD Selection Vectors**: Branchless query filtering generating index selection vectors with 0 CPU branch mispredictions.
- **Zero-Copy Python Arrow Interop**: Exporting C buffers directly to PyArrow / Polars / Pandas in 0.00ms via the Arrow C Data Interface.
- **Ultra-Lean Codebase**: Total engine targeted under **~2,500 Lines of Code (LOC)** in C11.

---

## 2. Project Documentation & Daily Diary Index

- **[day0.md](day0.md)** — *Day 0: Motivations, Technical Inspirations (ClickHouse, APL/K, DuckDB, Arrow), & Explicit Non-Goals*
- **[day1.md](day1.md)** — *Day 1: 64-Byte SIMD Memory Alignment & Dynamic Column Buffers*
- **[day2.md](day2.md)** — *Day 2: Zero-Copy Typed Slices & C11 `_Generic` Polymorphism*
- **[day3.md](day3.md)** — *Day 3: Zero-Copy Memory-Mapped Files (`mmap`) & OS Page Cache*
- **[day4.md](day4.md)** — *Day 4: Build System, Compiler Optimizations & POSIX Micro-Benchmarking*
- **[day5.md](day5.md)** — *Day 5: Column Vectors, Selection Vectors & Branchless Query Filtering*
- **[day6.md](day6.md)** — *Day 6: High-Cardinality String Dictionaries*
- **[day7.md](day7.md)** — *Day 7: Null Bitmaps & Dynamic Schema Registry*
- **[day8.md](day8.md)** — *Day 8: SIMD APL Vector Primitives (Compress & Reduction)*
- **[day9.md](day9.md)** — *Day 9: ClickHouse-Style On-Disk Column Layout & Marks Files (.mrk)*
- **[day10.md](day10.md)** — *Day 10: Sparse Primary Index (primary.idx) & 2-Tier Pruning*
- **[day11.md](day11.md)** — *Day 11: Column Reader (Zero-Copy Chunked Loading)*
- **[day12.md](day12.md)** — *Day 12: Inverted Index & BM25 Scoring*
- **[day13.md](day13.md)** — *Day 13: Roaring Bitset Deletion Vectors*
- **[day14.md](day14.md)** — *Day 14: The Table Abstraction (Ingestion Layer)*
- **[day14bis.md](day14bis.md)** — *Day 14 Bis: Table Flush & Immutable Part Serialization*
- **[day15.md](day15.md)** — *Day 15: External Merge Sort (Disk-Spilling ORDER BY)*
- **[plan.md](plan.md)** — *16-Day Architectural Master Plan, Python Productivization & Distributed Roadmap*
- **[editor.md](editor.md)** — *Zed & Neovim Editing Shortcut Cheat-Sheet*

---

## 3. Project Layout

```
cdb/
├── CMakeLists.txt              # Root C11 CMake build configuration
├── plan.md                     # Comprehensive 16-day architectural plan & curriculum (C11)
├── editor.md                   # Zed & Neovim editing cheat-sheet
├── day0.md                     # Day 0: Motivations, Inspirations & Non-Goals
├── day1.md                     # Day 1: 64-Byte SIMD Alignment & Dynamic Buffers
├── day2.md                     # Day 2: Zero-Copy Typed Slices & C11 _Generic
├── day3.md                     # Day 3: Zero-Copy Memory-Mapped Files (mmap)
├── day4.md                     # Day 4: Build System, Compiler Optimizations & POSIX Benchmarking
├── day5.md                     # Day 5: Column Vectors, Selection Vectors & Branchless Filtering
├── include/
│   └── simpledb/
│       ├── allocator.h         # SIMD Allocator & buffer_i32_t header
│       ├── slice.h             # Zero-copy slices & C11 _Generic macro
│       ├── mmap_file.h         # Zero-copy mmap file handle header
│       ├── benchmark.h         # High-resolution benchmark timing & PREVENT_DCE macro
│       └── db.h                # Database C API header
├── src/
│   ├── allocator.c             # Aligned malloc & buffer_i32_grow implementation
│   ├── mmap_file.c             # POSIX open, fstat, mmap, madvise implementation
│   ├── db.c                    # Database core implementation
│   └── main.c                  # REPL / CLI main executable
├── benchmarks/
│   └── bench_db.c              # Standalone hardware scan micro-benchmark suite
└── tests/
    ├── CMakeLists.txt          # CTest test configuration
    └── test_db.c               # C unit test runner
```

---

## 4. Building and Running

### Requirements
- C11 compatible compiler (Apple Clang, GCC 9+, Clang 11+)
- CMake 3.20+
- POSIX compliant OS (macOS or Linux)

### Quick Start
```bash
# Configure and build
cmake -B build
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure

# Run standalone benchmark suite
./build/bench_db

# Run executable CLI
./build/simpledb --version
```
