# Day 20: The MergeTree Naming Convention

You make an excellent point. Naming directories `part_<timestamp>` can lead to collisions if inserts happen rapidly within the same second, and it provides very little information about what's actually *inside* the part or how it relates to other parts. 

If we look at **ClickHouse's MergeTree engine**, it solves this elegantly using a very specific nomenclature that makes it possible to understand the lineage of a part just by looking at its directory name.

## The ClickHouse Pattern

A typical ClickHouse part name looks like this:
`{partition_id}_{min_block}_{max_block}_{level}`

1. **`partition_id`**: For partitioned tables (e.g. by month `202310`), this groups data. Since we don't have partitioning yet, we'll use a hardcoded prefix like `"all"`.
2. **`min_block`**: A monotonic ID assigned to the first data block that was merged to create this part.
3. **`max_block`**: A monotonic ID assigned to the last data block that was merged into this part.
4. **`level`**: The depth of the merge tree. Freshly inserted parts start at `level = 0`. When you merge a bunch of parts together, the new part's level is `max(levels) + 1`.

### Example Lineage

1. **Insert Batch 1**: We increment the table's `next_block_id` to `1`. The part is created as `all_1_1_0`.
2. **Insert Batch 2**: `next_block_id` becomes `2`. The part is `all_2_2_0`.
3. **Insert Batch 3**: `next_block_id` becomes `3`. The part is `all_3_3_0`.
4. **Compaction**: The background merger wakes up and selects `all_1_1_0` and `all_2_2_0`. 
   - `min_block` = `min(1, 2)` = `1`
   - `max_block` = `max(1, 2)` = `2`
   - `level` = `max(0, 0) + 1` = `1`
   - The new consolidated directory is named: `all_1_2_1`.

This scheme instantly tells us that `all_1_2_1` contains all the data from block 1 to block 2, and it has undergone 1 merge operation. It also makes resolving overlapping data extremely easy: if a query sees both `all_1_1_0` and `all_1_2_1` on disk (perhaps before the GC cleans up the zombie), it knows `all_1_2_1` *completely covers* the older part, allowing it to safely ignore it.

## Updating the C Headers

I've updated the `table_part_t` and `table_t` structs in `include/simpledb/table.h` to track these fields:

```c
// A simple structure representing an immutable flushed part on disk
typedef struct table_part_t {
    char part_path[256];
    size_t row_count;

    // ClickHouse MergeTree Naming Convention
    uint64_t min_block;
    uint64_t max_block;
    uint32_t level;

    // Atomic reference count for concurrent readers (0 = safe to delete)
    _Atomic size_t refcount;
} table_part_t;

typedef struct table_t {
    char name[64];
    schema_registry_t *schema;

    // Concurrency control for catalog metadata (parts array)
    pthread_rwlock_t catalog_lock;

    // In-memory columnar buffers (one per column in schema)
    void **column_buffers;

    // Total rows currently in memory
    size_t row_count;

    // Track flushed parts (for the Read API)
    table_part_t **parts;
    size_t num_parts;
    
    // Monotonic block sequence for ClickHouse-style part naming
    uint64_t next_block_id;

    // Optional FTS index for text columns
    inverted_index_t *fts;
} table_t;
```

## Your Turn

To fully adopt this, we'll need you to update the C implementation:
1. In `table.c`, modify `table_flush` to auto-increment `table->next_block_id`, assign the part's block/level metadata, and format the path string instead of relying strictly on the `data_dir` parameter.
2. In `part_merger.c`, modify `part_merger_execute` to calculate the `min_block`, `max_block`, and `level` based on the selected parts, and construct the new part directory name accordingly.

Are you ready to tackle the implementation?

## Implementation

As requested, I've implemented the `table_part_format_name` encapsulation directly into `simpledb/table.h`:

```c
static inline void table_part_format_name(char *buf, size_t buf_size, const char *base_dir, uint64_t min_block, uint64_t max_block, uint32_t level) {
    snprintf(buf, buf_size, "%s/all_%llu_%llu_%u", base_dir, (unsigned long long)min_block, (unsigned long long)max_block, (unsigned)level);
}
```

I also updated `table_flush` in `table.c` to auto-increment the `next_block_id` sequence, and updated `part_merger_execute` in `part_merger.c` to calculate the merge tree lineage for new parts:

```c
// part_merger.c snippet:

uint64_t new_min_block = UINT64_MAX;
uint64_t new_max_block = 0;
uint32_t max_level = 0;
for (size_t i = 0; i < merger->num_parts; i++) {
    table_part_t *p = merger->table->parts[merger->part_ids_to_merge[i]];
    if (p->min_block < new_min_block) new_min_block = p->min_block;
    if (p->max_block > new_max_block) new_max_block = p->max_block;
    if (p->level > max_level) max_level = p->level;
}
if (new_min_block == UINT64_MAX) new_min_block = 0;
uint32_t new_level = max_level + 1;

char new_part_dir[256];
table_part_format_name(new_part_dir, sizeof(new_part_dir), merger->table->name, new_min_block, new_max_block, new_level);
```

When we run the test suite, we can now see the exact ClickHouse folder structure physically manifesting on disk in the build directory!

```sh
build/tests/all_1_1_0
build/tests/all_2_2_0
build/tests/events_merge_test/all_1_2_1
```
