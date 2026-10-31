# Day 2: Zero-Copy Typed Slices & C11 `_Generic` Polymorphism

> **Day 2 Branch**: View the exact code for this day at [dmanchon/simpledb (`day2`)](https://github.com/dmanchon/simpledb/tree/day2) ([diff from `day1`](https://github.com/dmanchon/simpledb/compare/day1...day2)).

Building upon 64-byte aligned allocators, physical query execution operators require a mechanism to inspect column data without copying underlying memory or managing buffer lifecycles.

## 1. Memory Ownership in Query Execution

In object-oriented architectures, data movement between execution operators frequently involves vector allocation or copy construction. In an analytical engine scanning 100 million rows, copying column vectors introduces latency and consumes memory bandwidth.

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
#include "simpledb/allocator.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    const int32_t* data;
    size_t len;
} slice_i32_t;

typedef struct {
    const float* data;
    size_t len;
} slice_f32_t;

static inline slice_i32_t slice_i32_create(const int32_t* buff, size_t len) {
    return (slice_i32_t){ .data = buff, .len = len };
}

static inline slice_f32_t slice_f32_create(const float* buff, size_t len) {
    return (slice_f32_t){ .data = buff, .len = len };
}

static inline slice_i32_t buffer_i32_as_slice(const buffer_i32_t* buff) {
    if (!buff || !buff->data) return (slice_i32_t){ .data = NULL, .len = 0 };
    return (slice_i32_t){
        .data = (const int32_t*)buff->data,
        .len = buff->size
    };
}

static inline slice_f32_t buffer_f32_as_slice(const buffer_i32_t* buff) {
    if (!buff || !buff->data) return (slice_f32_t){ .data = NULL, .len = 0 };
    return (slice_f32_t){
        .data = (const float*)buff->data,
        .len = buff->size
    };
}

static inline slice_i32_t slice_i32_sub(slice_i32_t s, size_t offset, size_t count) {
    return (slice_i32_t){ .data = (const int32_t*)s.data + offset, .len = count };
}

static inline slice_f32_t slice_f32_sub(slice_f32_t s, size_t offset, size_t count) {
    return (slice_f32_t){ .data = (const float*)s.data + offset, .len = count };
}

static inline int32_t slice_i32_at(slice_i32_t s, size_t idx) {
    return s.data[idx];
}

static inline float slice_f32_at(slice_f32_t s, size_t idx) {
    return s.data[idx];
}

#define slice_of(PTR_OR_BUF, LEN) _Generic((PTR_OR_BUF), \
    int32_t*:       slice_i32_create((const int32_t*)(PTR_OR_BUF), (LEN)), \
    const int32_t*: slice_i32_create((const int32_t*)(PTR_OR_BUF), (LEN)), \
    float*:         slice_f32_create((const float*)(PTR_OR_BUF), (LEN)), \
    const float*:   slice_f32_create((const float*)(PTR_OR_BUF), (LEN)), \
    buffer_i32_t*:  buffer_i32_as_slice((const buffer_i32_t*)(PTR_OR_BUF)) \
)
```

---

## 6. Unit Test Verification

```c
// [tests/test_db.c]
static void test_slices(void) {
    // 1. Test NULL pointer slice creation via slice_of
    slice_i32_t null_slice = slice_of((int32_t*)NULL, 3);
    assert(null_slice.data == NULL);
    assert(null_slice.len == 3);
    (void)null_slice;

    // 2. Test buffer conversion & element lookup
    buffer_i32_t* buf = buffer_i32_create(16);
    for (int i = 0; i < 10; i++) {
        buffer_i32_push(buf, (i + 1) * 100);
    }

    slice_i32_t s_buf = slice_of(buf, 0);
    assert(s_buf.len == 10);
    assert(slice_i32_at(s_buf, 0) == 100);
    assert(slice_i32_at(s_buf, 4) == 500);
    (void)s_buf;

    // 3. Test O(1) sub-slicing
    slice_i32_t sub = slice_i32_sub(s_buf, 2, 5);
    assert(sub.len == 5);
    assert(slice_i32_at(sub, 0) == 300);
    assert(slice_i32_at(sub, 4) == 700);
    (void)sub;

    // 4. Test C11 _Generic macro dispatch for int32_t vs float arrays
    int32_t raw_i32[] = { 10, 20, 30 };
    float   raw_f32[] = { 1.5f, 2.5f, 3.5f };

    slice_i32_t gen_i32 = slice_of(raw_i32, 3);
    slice_f32_t gen_f32 = slice_of(raw_f32, 3);

    assert(gen_i32.len == 3 && slice_i32_at(gen_i32, 1) == 20);
    assert(gen_f32.len == 3 && slice_f32_at(gen_f32, 1) == 2.5f);
    (void)gen_i32;
    (void)gen_f32;

    // 5. Test buffer zero-copy re-interpretation as float slice
    slice_f32_t f32_from_buf = buffer_f32_as_slice(buf);
    assert(f32_from_buf.len == 10);
    assert(f32_from_buf.data != NULL);
    (void)f32_from_buf;

    buffer_i32_free(buf);
}
```

---

> **Day 2 Branch**: View the exact code for this day at [dmanchon/simpledb (`day2`)](https://github.com/dmanchon/simpledb/tree/day2) ([diff from `day1`](https://github.com/dmanchon/simpledb/compare/day1...day2)).
