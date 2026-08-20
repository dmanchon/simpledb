# Day 2: Zero-Copy Typed Slices and C11 `_Generic` Polymorphism

Building upon 64-byte aligned allocators, physical query execution operators require a mechanism to inspect column data without copying underlying memory or managing buffer lifecycles.

## 1. Memory Ownership in Query Execution

In object-oriented architectures, data movement between execution operators frequently involves vector allocation or copy construction. In an analytical engine scanning 100 million rows, copying column vectors introduces tens of milliseconds of latency and wastes hundreds of megabytes of memory bandwidth.

Physical query operators (such as SIMD filters, scans, and hash aggregations) read data sequentially without needing to modify or take ownership of the input columns.

```
                      Zero-Copy Window (O(1) Time, 0 Bytes Allocated)
                       ┌──────────────────────────────────────────────┐
                       │                                              │
Full Column Array:     [ 100 │ 200 │ 300 │ 400 │ 500 │ 600 │ 700 │ 800 ]
                                     ▲
                                     │  slice_i32_t { .data = ptr + 2, .len = 4 }
```

## 2. Zero-Copy Typed Slices

A slice provides a non-owning window view over a contiguous memory array.

```c
typedef struct {
    const int32_t* data;
    size_t len;
} slice_i32_t;
```

Slices offer three core structural benefits:
- O(1) Windowing: Sub-ranges of column data are created in constant time without memory allocation by advancing the pointer (`data + offset`) and adjusting `len`.
- Storage Agnosticism: Slices expose a uniform interface regardless of whether the target memory resides in an in-memory allocation buffer (`buffer_i32_t`) or a memory-mapped file descriptor (`mmap`).
- Read-Only Guarantees: The `const int32_t* data` qualifier prevents accidental modification of column data during query execution loops.

## 3. Polymorphism via C11 `_Generic` Macros

To support multiple column types (`int32_t`, `float`, `int64_t`) without type erasure (`void*`) or template instantiations, `simpledb` uses C11 `_Generic` selection expressions.

```c
#define slice_of(PTR, LEN) _Generic((PTR), \
    int32_t*:       slice_i32_create((const int32_t*)(PTR), (LEN)), \
    const int32_t*: slice_i32_create((const int32_t*)(PTR), (LEN)), \
    float*:         slice_f32_create((const float*)(PTR), (LEN)), \
    const float*:   slice_f32_create((const float*)(PTR), (LEN)) \
)
```

| Feature | C11 `_Generic` Macro | C++ Templates |
| :- | :- | :- |
| Compile-Time Safety | Unmatched types fail directly at macro expansion | Verbose template instantiation stack traces |
| Binary Code Size | Zero bloat (compiles only explicit functions) | Monomorphization bloat across object files |
| Runtime Overhead | Zero overhead (resolved entirely at compile time) | Zero overhead (unless virtual dispatch is used) |
| Language Complexity | Transparent C11 preprocessor expansion | Requires SFINAE, concepts, and meta-templates |

## 4. Architectural Comparison: Dynamic Buffer vs. Zero-Copy Slice

| Property | Dynamic Buffer (`buffer_i32_t`) | Zero-Copy Slice (`slice_i32_t`) |
| :- | :- | :- |
| Ownership | Owner (Manages heap allocation lifecycle) | View (Non-owning read-only reference) |
| Mutability | Mutable (Supports push and capacity growth) | Immutable (`const int32_t* data`) |
| Passing Overhead | Passed by pointer | Passed by value (16-24 bytes on stack) |
| Sub-Windowing | Requires memory allocation and copy | O(1) pointer arithmetic (`data + offset`) |
| Primary Role | Data ingestion and intermediate result buffers | Physical query operators and SIMD kernels |

## 5. Header Implementation: `include/simpledb/slice.h`

```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "simpledb/allocator.h"

typedef enum {
    COL_TYPE_I32,
    COL_TYPE_F32,
    COL_TYPE_I64,
    COL_TYPE_F64
} column_type_t;

typedef struct {
    const int32_t* data;
    size_t len;
} slice_i32_t;

typedef struct {
    const float* data;
    size_t len;
} slice_f32_t;

static inline slice_i32_t slice_i32_create(const int32_t* data, size_t len) {
    return (slice_i32_t){ .data = data, .len = len };
}

static inline slice_f32_t slice_f32_create(const float* data, size_t len) {
    return (slice_f32_t){ .data = data, .len = len };
}

static inline slice_i32_t buffer_i32_as_slice(const buffer_i32_t* buf) {
    if (!buf || !buf->data) return (slice_i32_t){ .data = NULL, .len = 0 };
    return (slice_i32_t){ .data = buf->data, .len = buf->size };
}

static inline slice_i32_t slice_i32_sub(slice_i32_t s, size_t offset, size_t count) {
    if (offset >= s.len) return (slice_i32_t){ .data = NULL, .len = 0 };
    if (offset + count > s.len) count = s.len - offset;
    return (slice_i32_t){ .data = s.data + offset, .len = count };
}

static inline int32_t slice_i32_at(slice_i32_t s, size_t idx) {
    assert(idx < s.len && "Slice index out of bounds");
    return s.data[idx];
}

#define slice_of(PTR, LEN) _Generic((PTR), \
    int32_t*:       slice_i32_create((const int32_t*)(PTR), (LEN)), \
    const int32_t*: slice_i32_create((const int32_t*)(PTR), (LEN)), \
    float*:         slice_f32_create((const float*)(PTR), (LEN)), \
    const float*:   slice_f32_create((const float*)(PTR), (LEN)) \
)
```

---

> **Source Code**: The full C11 codebase for this project is available on GitHub at [dmanchon/simpledb](https://github.com/dmanchon/simpledb).
