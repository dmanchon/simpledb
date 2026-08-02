# Building a Database Engine in C11 — Day 2: Zero-Copy Typed Slices & C11 `_Generic` Polymorphism

> **Series**: *Building `simpledb` — A High-Performance Columnar Engine in C11*  
> **Reading Time**: 7 minutes

---

On **Day 1**, we built 64-byte aligned dynamic memory allocators and heap array buffers (`buffer_i32_t`).

Today, on **Day 2**, we solve a critical architectural challenge in analytical database engines: **How do physical query operators read column data without copying bytes or taking memory ownership?**

We construct **Zero-Copy Typed Slices (`slice_i32_t`, `slice_f32_t`)**, **Column Type Enums (`column_type_t`)**, and **Compile-Time Polymorphism using C11 `_Generic` Macros**.

---

## 1. The Architectural Problem: Memory Ownership in Query Execution

In traditional object-oriented designs, passing data between execution operators often involves allocating vectors, copying buffers, or wrapping arrays in heavy class abstractions.

In a high-throughput columnar database scanning 100,000,000 rows:
* **Memory Copying is Unacceptable**: Copying a 100M-row column takes ~ 400 MB of memory bandwidth and tens of milliseconds of CPU stalls.
* **Ownership vs. Viewing**: Physical query operators (like SIMD filters, sequence scans, and hash aggregations) only need to **read** data. They should **never own or allocate memory** for input columns.

```
                      Zero-Copy Window (O(1) Time, 0 Bytes Allocated)
                       ┌──────────────────────────────────────────────┐
                       │                                              │
Full Column Array:     [ 100 │ 200 │ 300 │ 400 │ 500 │ 600 │ 700 │ 800 ]
                                     ▲
                                     │  slice_i32_t { .data = ptr + 2, .len = 4 }
```

---

## 2. Zero-Copy Slices (`slice_i32_t`)

A **Slice** is a lightweight, non-owning window view over a contiguous array:

```c
typedef struct {
    const int32_t* data;
    size_t len;
} slice_i32_t;
```

### Three Key Advantages of Slices:
1. **O(1) Sub-Slicing Window**: A sub-range of 1,000,000 rows can be windowed in **O(1) time and zero allocations** simply by incrementing the pointer (`data + offset`) and setting `len`.
2. **Unified Data Interface**: Slices work identically whether data originates from an in-memory `buffer_i32_t` or a zero-copy memory-mapped disk file (`mmap`).
3. **Immutability & Const Safety**: `const int32_t* data` prevents accidental data mutations during read-only query execution.

---

## 3. Polymorphism in C11: Why `_Generic` Beats C++ Templates

How do we write clean polymorphic code in C11 without C++ template metaprogramming or dangerous `void*` type erasure?

C11 introduced **`_Generic` expressions**, which evaluate argument types at compile-time to dispatch to the correct typed function:

```c
#define slice_of(PTR, LEN) _Generic((PTR), \
    int32_t*:       slice_i32_create((const int32_t*)(PTR), (LEN)), \
    const int32_t*: slice_i32_create((const int32_t*)(PTR), (LEN)), \
    float*:         slice_f32_create((const float*)(PTR), (LEN)), \
    const float*:   slice_f32_create((const float*)(PTR), (LEN)) \
)
```

### C11 `_Generic` vs. C++ Templates

| Feature | C11 `_Generic` Macro | C++ Templates |
| :--- | :--- | :--- |
| **Compile-Time Safety** | **Strict**: Unmatched types produce clean compile errors | **Verbose Errors**: Pages of template instantiation tracebacks |
| **Binary Code Size** | **Zero Bloat**: Only explicitly written code is compiled | **Code Bloat**: Duplicate template monomorphization bloat |
| **Runtime Overhead** | **0.00 ns**: Dispatches fully at compile time | **0.00 ns** (unless virtual functions are involved) |
| **Language Simplicity** | **Plain C11**: Transparent macro expansion | **Complex**: Requires SFINAE, concepts, and headers |

---

## 4. Architectural Comparison: Dynamic Buffer vs. Zero-Copy Slice

| Property | Dynamic Buffer (`buffer_i32_t`) | Zero-Copy Slice (`slice_i32_t`) |
| :--- | :--- | :--- |
| **Ownership** | **Owner** (Manages heap memory lifecycle) | **View** (Non-owning read-only reference) |
| **Mutability** | **Mutable** (Pushes & 2x capacity growth) | **Immutable** (`const int32_t* data`) |
| **Pass-by-Value** | Requires pointer (`buffer_i32_t*`) | Direct value (16--24 bytes, zero allocation) |
| **Sub-Window** | Requires copying bytes | O(1) pointer increment (`data + offset`) |
| **Use Case** | Data ingestion & intermediate results | Physical query operators & SIMD kernels |

---

## 5. Header Blueprint: `include/simpledb/slice.h`

```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "simpledb/allocator.h"

// 1. Column Data Types
typedef enum {
    TYPE_INT32,
    TYPE_FLOAT,
    TYPE_INT64,
    TYPE_DOUBLE
} column_type_t;

// 2. Typed Zero-Copy Slices
typedef struct {
    const int32_t* data;
    size_t len;
} slice_i32_t;

typedef struct {
    const float* data;
    size_t len;
} slice_f32_t;

// 3. Constructors & Sub-slicing
static inline slice_i32_t slice_i32_create(const int32_t* data, size_t len) {
    return (slice_i32_t){ .data = data, .len = len };
}

static inline slice_f32_t slice_f32_create(const float* data, size_t len) {
    return (slice_f32_t){ .data = data, .len = len };
}

// Convert buffer_i32_t to slice_i32_t (Zero-Copy View)
static inline slice_i32_t buffer_i32_as_slice(const buffer_i32_t* buf) {
    if (!buf || !buf->data) return (slice_i32_t){ .data = NULL, .len = 0 };
    return (slice_i32_t){ .data = buf->data, .len = buf->size };
}

// O(1) Sub-slicing (Window view)
static inline slice_i32_t slice_i32_sub(slice_i32_t s, size_t offset, size_t count) {
    if (offset >= s.len) return (slice_i32_t){ .data = NULL, .len = 0 };
    if (offset + count > s.len) count = s.len - offset;
    return (slice_i32_t){ .data = s.data + offset, .len = count };
}

// Element Access with Bounds Checking
static inline int32_t slice_i32_at(slice_i32_t s, size_t idx) {
    assert(idx < s.len && "Slice index out of bounds");
    return s.data[idx];
}

// 4. C11 Generic Macro Dispatch
#define slice_of(PTR, LEN) _Generic((PTR), \
    int32_t*:       slice_i32_create((const int32_t*)(PTR), (LEN)), \
    const int32_t*: slice_i32_create((const int32_t*)(PTR), (LEN)), \
    float*:         slice_f32_create((const float*)(PTR), (LEN)), \
    const float*:   slice_f32_create((const float*)(PTR), (LEN)) \
)
```

---

## Key Takeaways from Day 2

* **Zero-Copy Architecture**: Query operators must accept non-owning read-only window views (`slice_i32_t`) to avoid copying memory during physical query execution.
* **Compile-Time Polymorphism**: C11 `_Generic` expressions provide type-safe dispatch across `int32_t`, `float`, and `int64_t` with 0 nanoseconds runtime penalty.

*Next up in Day 3: Systems I/O & Zero-Copy Memory-Mapped Files (`mmap`)!*
