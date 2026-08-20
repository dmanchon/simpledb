# Day 5: Column Vectors, Selection Vectors and Branchless Filtering

In analytical query processing, filtering operations determine which rows satisfy specific predicates. In `simpledb`, query filters rely on selection vectors (`selection_vector_t`) and branchless comparison kernels to eliminate branch misprediction penalties and defer column copying until late materialization.

---

## 1. Concrete Query Example: Late Materialization vs. Early Payload Copying

To understand why selection vectors exist, consider a database table containing 1,000,000 users across 50 columns (`user_id`, `age`, `email`, `address`, `bio`, ...), occupying 500 MB of memory.

Suppose a query executes:
```sql
SELECT user_id, email FROM users WHERE age > 60;
```
If only 1,000 users out of 1,000,000 satisfy `age > 60`, row engines and column engines execute this query in fundamentally different ways.

A row-oriented engine reads all 50 columns into RAM for every single row, evaluates the predicate, and for matching rows copies full 50-column tuples into an intermediate result buffer. This consumes 500 MB of memory bandwidth on columns (`address`, `bio`) the query never asked for.

A columnar engine with selection vectors works differently:

1. **Filter Phase (`filter_gt_i32`)**: Scans only the 4-byte `age` column (4 MB total scan). For matching rows, it appends only their row index offsets (`[14, 82, 105, 902...]`) into a compact integer array called a Selection Vector. Zero row tuples are copied.
2. **Gather Phase (`gather_i32`)**: Uses the selection vector indices to jump directly to matching entries in `user_id` and `email`, materializing only those rows.

```
Early Materialization:
Row Array ──► [ Filter Predicate ] ──► Copy Full Tuples to Output Buffer

Late Materialization:
Column Vector ──► [ filter_gt_i32 ] ──► Selection Vector [14, 82, 105, 902]
                                                │
                                                ▼
                                   gather_i32 Only Needed Columns
```

---

## 2. Reusing `buffer_i32_t` as `selection_vector_t`

Rather than creating a redundant struct and duplicating allocation logic, `simpledb` leverages the existing 64-byte aligned dynamic array buffer (`buffer_i32_t`) built on Day 1.

```c
// selection_vector_t is a semantic type alias for buffer_i32_t
typedef buffer_i32_t selection_vector_t;

static inline selection_vector_t* selection_vector_create(size_t capacity) {
    return buffer_i32_create(capacity);
}

static inline void selection_vector_free(selection_vector_t* sel) {
    buffer_i32_free(sel);
}

static inline void selection_vector_clear(selection_vector_t* sel) {
    if (sel) sel->size = 0;
}

static inline int32_t selection_vector_at(const selection_vector_t* sel, size_t idx) {
    return sel->data[idx];
}
```

Reusing `buffer_i32_t` directly means zero code duplication: `selection_vector_t` inherits 64-byte `aligned_malloc` alignment, 2x capacity growth, and the full inline push API without writing a single extra struct or allocator.

Row indices (`0` to `2.14 billion`) fit inside `int32_t`, so `selection_vector_t` and `buffer_i32_t` are binary identical and pass seamlessly between query operators.

---

## 3. Capacity Growth Before the Filter Loop

The filter loop writes row indices unconditionally on every iteration. This means the output buffer must have enough pre-allocated capacity to hold the worst case, that is, every row in `src` matches the predicate.

`buffer_i32_ensure_capacity` doubles capacity in a loop until sufficient space exists:

```c
void buffer_i32_ensure_capacity(buffer_i32_t *buf, size_t needed_capacity) {
    if (!buf || buf->capacity >= needed_capacity)
        return;
    size_t new_cap = (buf->capacity == 0) ? 16 : buf->capacity;
    while (new_cap < needed_capacity) {
        new_cap *= 2;
    }
    int32_t *new_data = (int32_t *)aligned_malloc(new_cap * sizeof(int32_t));
    if (buf->data) {
        if (buf->size > 0) {
            memcpy(new_data, buf->data, buf->size * sizeof(int32_t));
        }
        aligned_free(buf->data);
    }
    buf->data     = new_data;
    buf->capacity = new_cap;
}
```

A single call before the loop (`buffer_i32_ensure_capacity(out_sel, out_sel->size + src.len)`) guarantees zero allocations inside the hot path. This is the same pre-allocation strategy used by ClickHouse and DuckDB for their selection vector filter kernels.

Note that the guard frees `buf->data` unconditionally when it exists, regardless of whether `buf->size > 0`, to avoid a memory leak when a selection vector is cleared and reused.

---

## 4. Branchless Filter Kernel

Evaluating filter predicates using conditional branches (`if (data[i] > target)`) degrades execution on unpredictable data. When branch direction is random, CPU branch predictors mispredict targets, triggering 15 to 20 clock cycle pipeline flushes per miss.

Branchless selection kernels eliminate conditional jumps by converting the comparison directly into integer arithmetic:

```c
bool filter_gt_i32(slice_i32_t src, int32_t target, selection_vector_t *out_sel) {
    if (!src.data || !out_sel)
        return false;

    // worst case scenario: all rows match, need room for existing + new
    buffer_i32_ensure_capacity(out_sel, out_sel->size + src.len);

    for (size_t i = 0; i < src.len; i++) {
        bool match               = src.data[i] > target;
        out_sel->data[out_sel->size] = (int32_t)i;
        out_sel->size           += match;
    }
    return true;
}
```

`bool match = (src.data[i] > target)` evaluates to integer `1` or `0`. The index `i` is written unconditionally into `out_sel->data[out_sel->size]`. Then `out_sel->size += match` advances the size pointer by 1 on a match, or by 0 on a miss. The next iteration overwrites the non-matching slot. The CPU executes every iteration at the same cost regardless of predicate selectivity.

---

## 5. Zero-Copy Gather Operator

Once a selection vector is generated, the gather operator extracts the column values at those row offsets into a compact output buffer.

```c
bool gather_i32(slice_i32_t src, const selection_vector_t *sel, buffer_i32_t *out_buf) {
    if (!src.data || !sel || !out_buf)
        return false;

    buffer_i32_ensure_capacity(out_buf, sel->size);

    for (size_t i = 0; i < sel->size; i++) {
        buffer_i32_push(out_buf, slice_i32_at(src, (size_t)sel->data[i]));
    }
    return true;
}
```

`sel->data[i]` contains a row index offset (e.g. `82`). `slice_i32_at(src, 82)` jumps directly to that position in the source column, reading only the matching row. Columns not referenced in the query projection are never touched.

`buffer_i32_ensure_capacity(out_buf, sel->size)` pre-allocates exactly `sel->size` slots before the loop. `buffer_i32_push` then appends each gathered value, inheriting the same dynamic growth infrastructure as every other buffer in `simpledb`. No special-cased allocation logic is needed.

---

## 6. Architectural Execution Metrics and Bottlenecks

The efficiency of physical query filtering depends on how effectively an algorithm minimizes CPU pipeline stalls and DRAM bandwidth consumption.

| Filtering Strategy | Predicate Selectivity | Expected Speedup Range | Dominant Hardware Bottleneck |
| :- | :- | :- | :- |
| Early Materialization (Tuple Copying) | 50% | Baseline (1.0x) | DRAM Memory Bandwidth (Payload Copying) |
| Conditional Branching (`if` Statements) | 50% (Unpredictable Data) | ~1.2x - 1.5x | CPU Instruction Pipeline Flushes (Branch Mispredictions) |
| Branchless Selection Vector | 50% (Unpredictable Data) | ~5x - 8x | Out-of-Order Instruction Dispatch Limits |
| SIMD Bitmask Vector Compression | 50% (Unpredictable Data) | ~15x - 20x+ | Bus Memory Bandwidth Saturation (~40-80 GB/s) |

This SIMD bitmask vector compression technique relies on advanced hardware instructions, which will be implemented and evaluated in Day 8.

---

## 7. How Selection Vectors Enable Long-Term Engine Architecture (Days 6-16)

Establishing `selection_vector_t` on Day 5 provides the foundational building block for advanced query execution across subsequent architectural phases.

**Multi-Predicate Filter Chaining (Days 6-8)**: For multi-clause queries (`WHERE age > 30 AND department_id = 42 AND status = 1`), consecutive filter operators do not scan raw column arrays multiple times. The first filter generates a selection vector. Subsequent filters evaluate predicates directly over the indexed offsets, scanning only matching rows rather than the full column.

**ClickHouse-Style Granule Pruning and Primary Key Marks (Days 9-12)**: When scanning MergeTree storage parts, sparse primary index mark files isolate relevant 8,192-row granules. Selection vectors represent active matching row offsets within granules without allocating intermediate table memory.

**Late Materialization in Projection, Hash Joins and Aggregations (Days 13-16)**: In queries referencing wide tables, the engine evaluates cheap numeric predicates first. Heavy string dictionary attributes and payload columns are gathered only for matching rows at the final query stage, reducing DRAM bandwidth consumption from hundreds of megabytes to a few kilobytes.

---

> **Source Code**: The full C11 codebase for this project is available on GitHub at [dmanchon/simpledb](https://github.com/dmanchon/simpledb).
