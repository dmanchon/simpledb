
Welcome to Day 11! Over the last two days, we built the **Write-Path** for our database. In Day 9, we built the `column_writer_t` to stream data into `.bin` files in 8,192-row chunks (Granules). In Day 10, we built the Sparse Primary Index to quickly prune those Granules, outputting a `bitmap_t` where a `1` means "Read this Granule" and a `0` means "Skip it."

Today, we build the first piece of our **Read-Path**: The Column Reader.

## 1. The Missing Link

If a user runs `SELECT SUM(salary) FROM employees WHERE id = 50000;`, our database does the following:
1. Passes `50000` to the `sparse_index_search` function.
2. The index returns a `bitmap_t` (e.g., `[0, 1, 0, 0]`, meaning only Granule 1 contains the target).
3. **Now what?**

We need a system that opens the `salary.bin` file, looks at that bitmap, completely skips Granules 0, 2, and 3, and only loads Granule 1 into CPU memory so we can run our SIMD `sum` function on it.

This system is the `column_reader_t`.

## 2. Zero-Copy Loading (The Power of mmap)

In older databases, reading data meant allocating massive arrays in RAM and calling `fread()` to physically copy bytes from the hard drive into memory. This is called **User-Space Copying**, and it is painfully slow.

Modern OLAP databases (like ClickHouse and DuckDB) use **Memory-Mapped Files (`mmap`)**. 
When you `mmap` a file, the OS doesn't actually read the file into RAM immediately. Instead, it gives you a pointer to a virtual address space that *represents* the file. As you access the pointer, the OS automatically pages the data from the SSD into RAM behind the scenes (via Page Faults), bypassing the user-space buffer entirely. This is called **Zero-Copy**.

Back in Day 3, we built the `mmap_file_t` wrapper. Today, we will use it as the engine for our Column Reader.

## 3. The Vectorized Yield (Pull Architecture)

How should our reader hand data to the execution engine?
Should it load all matching Granules at once and return a massive array? No! That would blow up our RAM.

Instead, modern execution engines use a **Pull Architecture** (specifically, the Vectorized Volcano Model). The execution engine asks the reader: *"Give me the next chunk of data."* 
The reader yields exactly one Granule (8,192 rows), the engine processes it, and then asks for the next. This keeps memory usage strictly O(1) during query execution!

## 4. The Architecture (storage.h)

Let's add the `column_reader_t` to our `include/simpledb/storage.h` file:

```c
// include/simpledb/storage.h (Additions)

#include "simpledb/mmap_file.h"
#include "simpledb/slice.h"
#include "simpledb/bitmap.h"

typedef struct {
    mmap_file_t* mmap;          // The memory-mapped .bin file
    column_type_t type;         // I32, F32, etc.
    const bitmap_t* mask;       // The mask from the Sparse Index!
    size_t total_granules;      // Total granules in the file
    size_t current_granule_idx; // Iterator state
} column_reader_t;

// 1. Open a column for reading. 
// If mask is NULL, it assumes a full table scan (reads everything).
column_reader_t* column_reader_create(const char* part_dir, const char* col_name, column_type_t type, const bitmap_t* mask);

// 2. Fetch the next matching Granule. 
// Returns a slice_i32_t. If len == 0, we have reached the end of the file.
slice_i32_t column_reader_next_granule_i32(column_reader_t* reader);

// 3. Clean up
void column_reader_free(column_reader_t* reader);
```

## 5. The Implementation Guide (storage.c)

Here is how you will implement this in `src/storage.c`:

### `column_reader_create`
1. Use `snprintf` to build the `.bin` path (`part_dir/col_name.bin`).
2. Call `mmap_file_open()` on that path.
3. Calculate `total_granules`. Since every row is a 4-byte integer (for now), the math is simply: `mmap->size / (8192 * sizeof(int32_t))`. (Note: The last granule might not be completely full, so you should round up!)
4. Store the `mask` pointer. (The reader doesn't own the mask, it just borrows it from the index!)

### `column_reader_next_granule_i32`
This is the heart of the engine. It is a `while` loop that advances `current_granule_idx` until it finds a Granule where the bit in the `mask` is `1`.
1. Check if `current_granule_idx >= total_granules`. If so, return an empty slice (len = 0).
2. If `mask` is provided, check `bitmap_is_set(mask, current_granule_idx)`. 
   - If it's `0`, skip it! Increment `idx` and `continue`.
3. If it's `1` (or if `mask` is NULL for a full table scan), we've found our chunk!
4. **The Zero-Copy mechanism:** Calculate the memory pointer: `const int32_t* ptr = ((const int32_t*)mmap->data) + (current_granule_idx * 8192);`
5. Calculate the remaining rows (it will be 8192, unless it's the very last granule in the file which might be partially full).
6. Increment `current_granule_idx`.
7. Return `slice_of(ptr, rows)`.

## 6. Testing the Reader

Let's write a unit test to verify that our reader correctly skips data on disk!

```c
void test_column_reader(void) {
    // 1. Write a dummy file with exactly 3 Granules (24,576 rows)
    const char* col_name = "reader_test";
    column_writer_t* writer = column_writer_create(".", col_name, COL_TYPE_I32);
    int32_t* data = malloc(24576 * sizeof(int32_t));
    for(int i = 0; i < 24576; i++) data[i] = i; // 0 to 24575
    column_writer_append(writer, data, 24576);
    column_writer_close(writer);
    free(data);
    
    // 2. Create a Bitmap that ONLY selects Granule 1
    bitmap_t* mask = bitmap_create(1);
    bitmap_set(mask, 1);
    
    // 3. Open the Reader
    column_reader_t* reader = column_reader_create(".", col_name, COL_TYPE_I32, mask);
    
    // 4. Fetch the next Granule. 
    // It should SKIP Granule 0 and instantly return Granule 1!
    slice_i32_t slice = column_reader_next_granule_i32(reader);
    
    assert(slice.len == 8192);
    // The first element of Granule 1 should be 8192
    assert(slice_i32_at(slice, 0) == 8192);
    
    // 5. Fetch again. Granule 2 is masked out, so it should hit EOF!
    slice_i32_t empty = column_reader_next_granule_i32(reader);
    assert(empty.len == 0);
    
    // 6. Cleanup
    column_reader_free(reader);
    bitmap_free(mask);
    remove("./reader_test.bin");
}
```

This tiny system is exactly how massive analytical databases manage to scan terabytes of data without running out of RAM.
