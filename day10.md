# Day 10: The Sparse Primary Index (2-Tier Query Pruning)

Today, we tackle one of the most brilliant architectural optimizations in OLAP databases: **The Sparse Primary Index**.

In a traditional OLTP database like PostgreSQL or MySQL, primary keys are indexed using a **B-Tree** (often a dense index). In a dense index, every single row in the database has a pointer in the index. If you have 1 billion rows, your index contains 1 billion entries. These dense indexes often consume hundreds of gigabytes of RAM, drastically inflating database operating costs.

But in analytical databases (OLAP), we are usually dealing with massive scale and running aggregations across millions of rows. Storing a pointer for every single row is a colossal waste of memory. 

ClickHouse solved this elegantly with **Sparse Indexing**.

---

## 1. The Sparse Index Concept

Recall from Day 9 that our data is physically streamed into `.bin` files and logically chunked into 8,192-row **Granules**.

Because we are writing analytical blocks (Parts), we can easily enforce that the data is **sorted** by a Primary Key before it is written to disk.

If the data is sorted, we do not need an index entry for every single row. Instead, we only need **one index entry per Granule**. Specifically, we store the *minimum* primary key value found at the start of that Granule.

### The Math of Memory Savings

If we have 1,000,000,000 (One Billion) rows:
- A Dense B-Tree index (assuming 8 bytes per entry) would consume **8 Gigabytes** of RAM just for the index.
- A Sparse Index with a granularity of 8,192 rows would only need $1,000,000,000 / 8192 = 122,070$ entries. At 4 bytes per entry, the entire index consumes **488 Kilobytes**.

This is an **$8,192\times$ reduction in memory**. A billion-row index can now fit entirely within a modern CPU's L2 cache!

---

## 2. The 2-Tier Pruner

When a user runs a query like `SELECT * FROM table WHERE user_id = 50000`, we need to find which Granules to read. We do this using a 2-Tier Pruning system.

### Tier 1: Part-Level Pruning
Before we even look at the Granules, we track the absolute `min` and `max` primary key value for the *entire* Part.
If a Part contains `user_id`s from `0` to `40000`, and the query asks for `50000`, we can instantly skip the entire Part (and entirely skip searching its index). 

### Tier 2: Granule-Level Pruning (Binary Search)
If the query falls within the Part's range, we load the `.idx` file (an array of our Granule marks) into memory. Because the marks are sorted, we can perform a highly efficient **Binary Search** over the array to find exactly which Granule contains the target value.

For example, if our `.idx` file contains:
`[0, 81920, 163840, 245760]`

And we search for `user_id = 100000`:
1. Binary search finds that `100000` is greater than `81920` (Granule 1), but less than `163840` (Granule 2).
2. Therefore, `user_id = 100000` *must* live inside Granule 1.
3. The Pruner returns `{ .start_granule = 1, .end_granule = 2 }`.

Instead of scanning the whole `.bin` file, our execution engine will use pure math (from Day 9) to `fseek` directly to Granule 1 and only read those 8,192 rows.

---

## 3. The Headers

We have drafted the data structures in `include/simpledb/sparse_index.h`:

```c
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Represents a contiguous range of granules to scan
typedef struct {
    uint32_t start_granule; // Inclusive
    uint32_t end_granule;   // Exclusive
} granule_range_t;

// The in-memory representation of the primary.idx file
typedef struct {
    int32_t* marks;     // Array of primary key values, one per granule
    size_t num_marks;   // Number of granules
    int32_t min_val;    // Absolute min for the entire part
    int32_t max_val;    // Absolute max for the entire part
} sparse_index_t;

// Load a sparse index from disk
sparse_index_t* sparse_index_load(const char* idx_file_path);

// Free the index
void sparse_index_free(sparse_index_t* idx);

// 2-Tier Pruning logic
bool sparse_index_search(const sparse_index_t* idx, int32_t target_val, granule_range_t* out_range);
```

## Socratic Question

We are going to implement the actual logic for this next. Take a look at the `sparse_index_search` function signature. 

Currently, it takes a single `target_val` (e.g., `user_id = X`). But what happens when the user runs a range query like `user_id BETWEEN 100 AND 500`? Does the current `granule_range_t` structure give us enough flexibility to return multiple contiguous granules? What modifications would we need to support range queries?
