# Building a Database Engine in C11 — Day 1: 64-Byte SIMD Alignment & Dynamic Column Buffers

> **Series**: *Building `simpledb` — A High-Performance Columnar Engine in C11*  
> **Reading Time**: 8 minutes

---

Why do modern analytical databases like **ClickHouse**, **DuckDB**, and **MonetDB** scan billions of rows per second, while traditional row-oriented databases crawl under large aggregation queries?

The secret lies at the hardware level: **cache-line layout, memory alignment, and SIMD vector execution**.

In this post, we kick off **Day 1** of building `simpledb`—a high-performance columnar engine written in **Plain C11**. We dive into the low-level foundation: why 64-byte memory alignment is non-negotiable for SIMD hardware, and how we construct zero-overhead dynamic array buffers (`buffer_i32_t`).

---

## 1. Why Plain C11 for an Analytical Database Core?

When building database kernels, C11 offers distinct mechanical advantages over higher-level languages:

1. **Zero Template & Vtable Bloat**: No virtual method tables (`vtable`), no hidden `std::allocator_traits` indirections, and no compiler-generated template instantiation code bloat.
2. **Explicit Memory Layout Control**: Every byte, pointer, alignment boundary, and allocation lifetime is completely explicit and predictable.
3. **Direct Hardware & OS Mapping**: Direct use of `posix_memalign`, `mmap`, `madvise`, and native vector intrinsics (ARM Neon / Intel AVX-512) over flat typed array pointers (`int32_t*`, `float*`).
4. **Follows Proven Systems Engines**: Industry giants like **Redis**, the **Linux kernel**, **SQLite**, **DuckDB's execution core**, and **MonetDB** are engineered in plain C.

---

## 2. The Hardware Problem: Why 64-Byte Alignment?

Modern CPUs do not fetch individual bytes from RAM. They load data in **64-byte blocks** called **Cache Lines**.

Furthermore, modern vector registers operate on wide SIMD lanes:
* **ARM Neon (Apple Silicon)**: 128-bit registers ($4 \times \text{int32\_t}$).
* **Intel AVX2**: 256-bit registers ($8 \times \text{int32\_t}$).
* **Intel AVX-512**: 512-bit registers ($16 \times \text{int32\_t} = \mathbf{64\text{ \textbf{bytes}}}$).

```
   Memory Address: 0x1000 (Aligned to 64 bytes)
   ┌───────────────────────────────────────────────────────────────┐
   │ [0] │ [1] │ [2] │ [3] │ ... │ [12] │ [13] │ [14] │ [15]       │  <-- 1 Single Cache Line Fetch (AVX-512)
   └───────────────────────────────────────────────────────────────┘
```

### The Penalty of Unaligned Memory
If a vector array starts at an unaligned memory address (e.g. `0x1003` instead of `0x1000`):
1. **Cache-Line Split Penalty**: A single 64-byte SIMD vector load spans across **two physical cache lines**, forcing the memory controller to perform two DRAM fetches instead of one.
2. **CPU Pipeline Stalls**: Unaligned vector loads cause memory bus locks and pipeline stalls, degrading throughput by $30\%\text{--}50\%$.

To prevent cache-line splits, every column array in `simpledb` MUST start on a memory address divisible by 64 (`address % 64 == 0`).

---

## 3. The Allocator Engine: `aligned_malloc` & `aligned_free`

In POSIX C11, `posix_memalign` is the native system interface to request hardware-aligned heap memory:

```c
int posix_memalign(void **memptr, size_t alignment, size_t size);
```

### Implementation ([src/allocator.c](file:///Users/dmanchon/repos/cdb/src/allocator.c))

```c
#include "simpledb/allocator.h"
#include <stdio.h>
#include <stdlib.h>

void* aligned_malloc(size_t bytes) {
    if (bytes == 0) return NULL;

    void* buffer = NULL;
    int status = posix_memalign(&buffer, SIMD_ALIGNMENT, bytes);

    if (status != 0) {
        fprintf(stderr, "[FATAL ERROR] aligned_malloc failed to allocate %zu bytes (code: %d)\n", bytes, status);
        exit(EXIT_FAILURE);
    }
    return buffer;
}

void aligned_free(void* ptr) {
    if (ptr) {
        free(ptr); // POSIX guarantees standard free() releases posix_memalign memory
    }
}
```

---

## 4. Contiguous Array Buffers (`buffer_i32_t`) vs. Linked Lists

Why do analytical column-stores store data in **contiguous dynamic array buffers (`buffer_i32_t`)** rather than linked lists?

```
Contiguous Buffer (buffer_i32_t):
[ 100 | 200 | 300 | 400 | 500 | 600 | 700 | 800 ]  <-- 1 Memory Fetch = 16 integers in L1 Cache

Linked List:
[Node 0] ──> [Node 1] ──> [Node 2] ──> [Node 3]     <-- 100ns RAM stall per pointer dereference!
```

### Architectural Comparison Matrix

| Architectural Feature | Contiguous Array Buffer (`buffer_i32_t`) | Linked List (`node_t*`) |
| :--- | :--- | :--- |
| **Memory Layout** | **100% Contiguous** in RAM | **Scattered / Fragmented** across heap |
| **CPU Cache Prefetching** | **Optimal**: 1 fetch loads $16 \times \text{int32\_t}$ into L1 cache | **Catastrophic**: Pointer-chasing causes L3/RAM cache misses |
| **SIMD Vectorization** | **Direct Vector Execution**: 4–16 values calculated per cycle | **IMPOSSIBLE**: Nodes separated by 64-bit pointers |
| **Memory Overhead** | **0% per-element overhead** ($4\text{ bytes}$ per `int32_t`) | **700% memory bloat** ($4\text{b value} + 8\text{b pointer} + 16\text{b header}$) |
| **Zero-Copy `mmap`** | **Native**: `mmap()` file directly casts to `int32_t*` pointer | **Requires Copying**: Cannot memory-map pointers directly |

### Why `int32_t` as the Primary Base Type? (Justified by ClickHouse & DuckDB)

Why start with 32-bit integers (`int32_t`) as our core vector type instead of `void*` or floating-point numbers?

Both **DuckDB** and **ClickHouse** use 32-bit integers as their universal indexing and execution foundation:
1. **DuckDB (`sel_t = uint32_t`)**: Every vector filter and join operation in DuckDB uses `sel_t` (`uint32_t`) selection vectors to index and reorder rows without copying payload bytes ([duckdb/selection_vector.hpp](https://github.com/duckdb/duckdb/blob/main/src/include/duckdb/common/types/selection_vector.hpp)).
2. **ClickHouse (`ColumnVector<Int32>`)**: ClickHouse's `ColumnInt32` manages 64-byte aligned SIMD arrays (`PaddedPODArray<Int32>`) for numeric columns and dictionary keys ([src/Columns/ColumnVector.h](https://github.com/ClickHouse/ClickHouse/blob/master/src/Columns/ColumnVector.h)).
3. **Dictionary Encoding (Text $\rightarrow$ `int32_t`)**: Analytical engines convert variable-length strings into 32-bit integer IDs (`"USA" -> 0, "Canada" -> 1`). String comparisons become ultra-fast SIMD `int32_t` array operations!
4. **2.14 Billion Row Range**: 32-bit integers cover up to **2.14 Billion rows per table block**, striking the perfect balance between high SIMD parallelism ($16 \times \text{int32\_t}$ per AVX-512 vector register) and memory capacity.

*(Note: On **Day 2**, we generalize our buffer layer with `column_type_t` and `slice_of` (`_Generic`), allowing the engine to handle `float`, `int64_t`, and `double` seamlessly!)*

---

## 5. Hot/Cold Separation in Vector Resizing

To ensure maximum performance in inner query loops, we strictly separate the **Hot Insertion Path** from the **Cold Resizing Path**:

```c
// [include/simpledb/allocator.h]
// Cold path function declaration (implemented out-of-line in src/allocator.c)
void buffer_i32_grow(buffer_i32_t* buf);

// Hot path: Inlined in header for 0 call overhead in tight query loops
static inline bool buffer_i32_push(buffer_i32_t* buf, int32_t val) {
    if (!buf) return false;

    if (buf->size >= buf->capacity) {
        buffer_i32_grow(buf); // Only calls out-of-line helper when capacity is full!
    }

    buf->data[buf->size++] = val;
    return true;
}

// [src/allocator.c]
// Cold path implementation: Out-of-line to keep the CPU Instruction Cache (I-Cache) ultra-lean
void buffer_i32_grow(buffer_i32_t* buf) {
    size_t new_capacity = (buf->capacity == 0) ? 16 : buf->capacity * 2;
    int32_t* new_data = (int32_t*) aligned_malloc(new_capacity * sizeof(int32_t));
    if (!new_data) {
        fprintf(stderr, "[FATAL ERROR] cannot grow buffer from %zu -> %zu\n", buf->size, new_capacity);
        exit(EXIT_FAILURE);
    }
    if (buf->data && buf->size > 0) {
        // Fast SIMD block memory copy
        memcpy(new_data, buf->data, buf->size * sizeof(int32_t));
        aligned_free(buf->data);
    }
    buf->data = new_data;
    buf->capacity = new_capacity;
}
```

---

## 6. Verification & Automated Testing

We verify our implementation with automated C unit tests in [tests/test_db.c](file:///Users/dmanchon/repos/cdb/tests/test_db.c):

1. **64-Byte Alignment Verification**:
   ```c
   uintptr_t addr = (uintptr_t) buf->data;
   assert((addr % 64) == 0 && "Buffer pointer MUST be 64-byte aligned!");
   ```
2. **Dynamic 2x Expansion Test**: Push 1,000+ integers to verify `memcpy` block copying.

```bash
$ cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
Test project /Users/dmanchon/repos/cdb/build
    Start 1: test_db
1/1 Test #1: test_db ..........................   Passed    0.36 sec

100% tests passed out of 1
```

---

## Key Takeaways from Day 1

* **Memory Alignment**: 64-byte alignment prevents CPU cache-line splits and enables peak SIMD register throughput.
* **Contiguous Memory**: Dynamic contiguous arrays beat linked lists by $7\times$ in memory efficiency and $10\times\text{--}50\times$ in CPU cache speed.
* **Code Architecture**: Separate hot inline functions (`buffer_i32_push`) from cold helpers (`buffer_i32_grow`) to keep CPU instruction caches (`I-Cache`) ultra-lean.

*Next up in Day 2: Zero-Copy Typed Slices (`slice_i32_t`) and C11 `_Generic` Macros!*
