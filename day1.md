# Day 1: 64-Byte SIMD Alignment and Dynamic Column Buffers

Analytical database engines like ClickHouse, DuckDB, and MonetDB scan billions of rows per second by aligning memory layouts with hardware cache architecture. `simpledb` uses 64-byte aligned dynamic column buffers.

---

## 1. Hardware Cache Lines and Memory Alignment

Modern CPUs do not fetch individual bytes from main memory. Data is transferred into cache hierarchies in 64-byte blocks called cache lines.

SIMD vector instruction sets operate on wide registers:
- ARM Neon (Apple Silicon): 128-bit registers (4 x int32_t)
- Intel AVX2: 256-bit registers (8 x int32_t)
- Intel AVX-512: 512-bit registers (16 x int32_t = 64 bytes)

```
   Memory Address: 0x1000 (Aligned to 64 bytes)
   ┌───────────────────────────────────────────────────────────────┐
   │ [0] │ [1] │ [2] │ [3] │ ... │ [12] │ [13] │ [14] │ [15]       │  <- 1 Cache Line Fetch
   └───────────────────────────────────────────────────────────────┘
```

If an array allocation starts at an unaligned memory address, a 64-byte SIMD vector load spans across two physical cache lines. This triggers a cache line split penalty, forcing the memory controller to issue two DRAM fetches and causing pipeline stalls that reduce scanning throughput.

To guarantee optimal vector instruction throughput, every column array in `simpledb` is aligned to a 64-byte boundary (`address % 64 == 0`).

---

## 2. Aligned Allocator Implementation

In POSIX C11, `posix_memalign` provides the standard interface for requesting aligned heap memory.

```c
// [src/allocator.c]
#include "simpledb/allocator.h"
#include <stdio.h>
#include <stdlib.h>

void* aligned_malloc(size_t bytes) {
    if (bytes == 0) return NULL;

    void* buffer = NULL;
    int status = posix_memalign(&buffer, SIMD_ALIGNMENT, bytes);

    if (status != 0) {
        fprintf(stderr, "[simpledb error] aligned_malloc failed to allocate %zu bytes (code: %d)\n", bytes, status);
        exit(EXIT_FAILURE);
    }
    return buffer;
}

void aligned_free(void* ptr) {
    if (ptr) {
        free(ptr);
    }
}
```

---

## 3. Contiguous Buffers vs. Pointer Layouts

Analytical engines use contiguous dynamic array buffers (`buffer_i32_t`) rather than linked structures or pointer-based objects.

```
Contiguous Buffer (buffer_i32_t):
[ 100 | 200 | 300 | 400 | 500 | 600 | 700 | 800 ]  <- 1 Memory Fetch = 16 integers in L1 Cache

Linked List:
[Node 0] ──> [Node 1] ──> [Node 2] ──> [Node 3]     <- RAM stall per pointer dereference
```

| Architectural Feature | Contiguous Array Buffer (`buffer_i32_t`) | Linked List (`node_t*`) |
| :- | :- | :- |
| Memory Layout | 100% Contiguous in RAM | Scattered across heap |
| CPU Cache Prefetching | 1 fetch loads 16 x int32_t into L1 | Pointer chasing causes L3/RAM misses |
| SIMD Vectorization | Direct vector execution (4-16 values/cycle) | Impossible due to 64-bit pointer gaps |
| Memory Overhead | 0% per-element overhead (4 bytes/element) | High overhead (pointer + metadata bloat) |
| Zero-Copy mmap | Native: `mmap()` file directly casts to pointer | Requires full copy and pointer swizzling |

32-bit integers (`int32_t`) serve as the baseline primitive type for column vectors. Both DuckDB (`sel_t = uint32_t`) and ClickHouse (`ColumnVector<Int32>`) rely on 32-bit integer representations for selection vectors and dictionary-encoded string columns.

---

## 4. Hot/Cold Path Separation in Vector Resizing

To preserve instruction cache (I-Cache) density during execution loops, vector push operations separate the inline hot path from the out-of-line resizing logic.

```c
// [include/simpledb/allocator.h]
typedef struct {
    int32_t* data;
    size_t size;
    size_t capacity;
} buffer_i32_t;

void buffer_i32_grow(buffer_i32_t* buf);

// Inlined hot path for element insertion
static inline bool buffer_i32_push(buffer_i32_t* buf, int32_t val) {
    if (!buf) return false;

    if (buf->size >= buf->capacity) {
        buffer_i32_grow(buf);
    }

    buf->data[buf->size++] = val;
    return true;
}

// [src/allocator.c]
// Out-of-line cold path for capacity growth
void buffer_i32_grow(buffer_i32_t* buf) {
    size_t new_capacity = (buf->capacity == 0) ? 16 : buf->capacity * 2;
    int32_t* new_data = (int32_t*) aligned_malloc(new_capacity * sizeof(int32_t));
    if (!new_data) {
        fprintf(stderr, "[simpledb error] Cannot grow buffer from %zu to %zu\n", buf->size, new_capacity);
        exit(EXIT_FAILURE);
    }
    if (buf->data && buf->size > 0) {
        memcpy(new_data, buf->data, buf->size * sizeof(int32_t));
        aligned_free(buf->data);
    }
    buf->data = new_data;
    buf->capacity = new_capacity;
}
```

By separating `buffer_i32_grow` into a standalone function in `src/allocator.c`, the generated assembly inside `buffer_i32_push` remains compact, allowing CPU instruction caches to hold inner loop bodies without instruction eviction.

---

> **Source Code**: The full C11 codebase for this project is available on GitHub at [dmanchon/simpledb](https://github.com/dmanchon/simpledb).
