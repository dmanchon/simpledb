# `simpledb` — Low-Level Columnar Analytical Database Engine (C11 Edition)

`simpledb` is a high-performance, single-node, out-of-core columnar analytical database engine written from scratch in **Modern C11**.

---

## 1. Architectural Highlights

- **Zero Template Overhead**: Built strictly in C11 (`-std=c11`) with zero C++ template metaprogramming, vtables, or header bloat.
- **64-Byte SIMD Memory Alignment**: Raw aligned memory allocations (`aligned_malloc`) matching 64-byte hardware cache line boundaries (ARM Neon / Intel AVX2 / AVX-512).
- **ClickHouse-Style Sparse Primary Indexing**: 8,192-row Granule block partitioning with Marks Files (`.mrk`) reducing primary index size by $8,192\times$.
- **APL & K/q Vector Primitives**: Dynamic array buffers (`buffer_i32_t`) executing bulk array operations (`compress`, `sum`, `max`, `gather`, `group`).
- **Zero-Copy Memory-Mapped Storage**: `mmap` binary file format reading columns directly into memory pointers.
- **Zero-Copy Python Arrow Interop**: Exporting C buffers directly to PyArrow / Polars / Pandas in 0.00ms via the Arrow C Data Interface.
- **Ultra-Lean Codebase**: Total engine targeted under **~2,500 Lines of Code (LOC)** in C11.

---

## 2. Project Documentation & Daily Diary Index

- **[day0.md](day0.md)** — *Day 0: Motivations, Technical Inspirations (ClickHouse, APL/K, DuckDB, Arrow), & Explicit Non-Goals*
- **[day1.md](day1.md)** — *Day 1: 64-Byte SIMD Memory Alignment & Dynamic Column Buffers*
- **[day2.md](day2.md)** — *Day 2: Zero-Copy Typed Slices & C11 `_Generic` Polymorphism*
- **[day3.md](day3.md)** — *Day 3: Zero-Copy Memory-Mapped Files (`mmap`) & OS Page Cache*
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
├── include/
│   └── simpledb/
│       ├── allocator.h         # SIMD Allocator & buffer_i32_t header
│       ├── slice.h             # Zero-copy slices & C11 _Generic macro
│       └── db.h                # Database C API header
├── src/
│   ├── allocator.c             # Aligned malloc & buffer_i32_grow implementation
│   ├── db.c                    # Database core implementation
│   └── main.c                  # REPL / CLI main executable
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

# Run executable CLI
./build/simpledb --version
```
