# Day 11: The Column Reader (Zero-Copy Data Loading)

Welcome to Day 11! Over the last two days, we built the **Write-Path** for our database. In Day 9, we built the `column_writer_t` to stream data into `.bin` files in 8,192-row chunks (Granules). In Day 10, we built the Sparse Primary Index to quickly prune those Granules, outputting a `bitmap_t` where a `1` means "Read this Granule" and a `0` means "Skip it."

Today, we build the first piece of our **Read-Path**: The Column Reader.

## 1. The Missing Link

If a user runs `SELECT SUM(salary) FROM employees WHERE id = 50000;`, our database does the following:
1. Passes `50000` to the `sparse_index_search` function.
2. The index returns a `bitmap_t` (e.g., `[0, 1, 0, 0]`, meaning only Granule 1 contains the target).
3. **Now what?**

We need a system that opens the `salary.bin` file, looks at that bitmap, completely skips Granules 0, 2, and 3, and only loads Granule 1 into CPU memory so we can run our SIMD `sum` function on it.

This system is the `column_reader_t`.

## 2. How the Giants Do It: Column Readers in SOTA Engines

To understand where our `column_reader_t` fits in the broader industry, let's look at how the State-of-the-Art (SOTA) analytical databases handle data loading.

### DuckDB (Vectorized Volcano)
DuckDB pioneered bringing the Vectorized Volcano execution model to embedded analytics. In DuckDB, the execution engine sits at the top of a tree and "pulls" data upward. The leaf node of this tree is the Table Scan operator. When pulled, the DuckDB scanner reads compressed blocks from disk, decompresses them into exactly **2048-row Vectors** (their standard vector size), and yields them up to the filter and aggregation nodes. They rely heavily on Min-Max Zone Maps to aggressively skip blocks before decompression.

### ClickHouse (Push-Based Pipelines)
ClickHouse originally used a Pull model (like DuckDB and `simpledb`), but rewrote their core execution engine to use a **Push-based Pipeline**. Instead of the top node asking for data, the Column Reader threads asynchronously race ahead, pre-fetching Granules (typically 8,192 rows) from SSD, decompressing them (using LZ4 or Zstd), and "pushing" these blocks downstream into a queue for the execution threads to process. This maximizes CPU and I/O saturation on  multi-core servers.

### Apache Arrow & LanceDB (Zero-Copy & Direct SIMD)
Newer data lakes and AI-focused columnar databases (like LanceDB) take the concept of **Zero-Copy** to the extreme. Like `simpledb`, they use `mmap` to map files directly into memory. However, instead of decompressing a block into a RAM buffer before processing, they use advanced encodings (like Dictionary Encoding or Bit-Packing) that allow SIMD instructions to operate *directly on the compressed bytes*.

## 3. Zero-Copy Loading (The Power of mmap)

In older databases, reading data meant allocating  arrays in RAM (e.g., `malloc(1024)`) and calling `read()`. The Operating System would read data from the SSD into its own internal "Kernel Page Cache", and then physically copy that data *again* into your user-space C buffer. This double-copy architecture burns CPU cycles and needlessly inflates RAM usage.

Modern OLAP databases (like ClickHouse and DuckDB) use **Memory-Mapped Files (`mmap`)**. 

When you `mmap` a file, you ask the OS for a virtual memory pointer (e.g., `0x7ffee...`). The OS does not actually read the file into RAM immediately. Instead, it maps the file's blocks to that address space. When your C code tries to access `pointer[0]`, the CPU triggers a **Page Fault**. The OS catches this fault, fetches the exact 4KB block from the SSD into RAM, and hands it directly to your program. 

By doing this, we achieve **Zero-Copy**: there is no `malloc` buffer and no `read()` call. The OS Kernel Cache becomes our database's RAM cache, automatically evicting old pages when memory gets full.

### The Anatomy of `mmap` (The Syscalls)
To set up a memory mapping in C, we talk to the kernel using three POSIX system calls. First, we need to include the right headers:
```c
#include <fcntl.h>     // For open() and O_RDONLY
#include <sys/stat.h>  // For fstat()
#include <sys/mman.h>  // For mmap()
```

#### 1. `open()`
```c
int fd = open("column.bin", O_RDONLY);
```
The OS verifies the file exists and that you have read permissions. It then returns a **File Descriptor (FD)**, a simple integer (like `3`) that acts as a ticket representing your open file. We use `O_RDONLY` because our reader should never modify the immutable column data.

#### 2. `fstat()`
```c
struct stat st;
fstat(fd, &st);
size_t length = st.st_size;
```
We need to know exactly how many bytes to map into memory, so we ask the OS for the file's metadata (`stat`) using our file descriptor.

#### 3. `mmap()`
This is the  function. Here is how we call it and why:
```c
void* ptr = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
```
* **`addr` (`NULL`)**: We pass `NULL` to tell the OS, "You choose the virtual memory address. I don't care where it lives in my RAM."
* **`length`**: The size of the file we got from `fstat`.
* **`prot` (`PROT_READ`)**: Memory protection. We only want to read. The OS will enforce this and crash our program with a Segmentation Fault if we accidentally try to write to this pointer! (Note: The OS will only allow `PROT_READ` if the original `open` call used `O_RDONLY` or `O_RDWR`).
* **`flags` (`MAP_SHARED`)**: Tells the OS how to handle the mapping. `MAP_SHARED` means if another process modifies the file on disk, we will see those changes in our memory. (The alternative is `MAP_PRIVATE`, which creates a copy-on-write snapshot).
* **`fd`**: The file descriptor ticket we got from `open()`.
* **`offset` (`0`)**: Where in the file to start mapping. We pass `0` to map from the very beginning of the `.bin` file.

**A Note on Portability:**
The `mmap()` function is part of the POSIX standard, meaning it works natively on Unix-like systems (Linux, macOS, BSD). It does *not* exist on Windows. On Windows, the Win32 API uses an identical concept but different functions (`CreateFileMappingA()` and `MapViewOfFile()`). For a cross-platform database, you would use an `#ifdef _WIN32` block to swap the implementations. Since `simpledb` is built for modern Unix environments, we stick purely to the POSIX implementation.

Back in Day 3, we abstracted this entire Unix setup into a handy `mmap_file_t` wrapper struct, which we will use today as the core engine for our Column Reader.

## 4. The Vectorized Yield (Pull Architecture)

How should our reader hand data to the execution engine?
Should it load all matching Granules at once and return a massive array? No! That would blow up our RAM.

Instead, modern execution engines use a **Pull Architecture** (specifically, the Vectorized Volcano Model). The execution engine asks the reader: *"Give me the next chunk of data."* 
The reader yields exactly one Granule (8,192 rows), the engine processes it, and then asks for the next. This keeps memory usage strictly O(1) during query execution!

### Why loop over a bitmap instead of jumping directly?
You might wonder: if we already have a bitmap that tells us exactly which Granules we want, why bother with a `while` loop iterator? Why not just jump straight to the data?

1. **O(1) Memory Usage**: If a query matches 50,000 Granules (400 million rows) and we return them all at once, we force the OS to page-fault gigabytes of data into RAM simultaneously. By using an iterator, we process exactly one Granule at a time in the CPU's tight L2 cache, never exceeding a few kilobytes of RAM regardless of the dataset size.
2. **Nanosecond Bitmap Scanning**: The `while` loop does not loop over the database rows on the SSD; it loops over the bits in the `bitmap_t` in L1 cache. Checking a bit takes ~1 nanosecond. When the loop sees a `0`, it instantly skips it. Because we never touch the `mmap` pointer for skipped Granules, the CPU never triggers a Page Fault, and we skip the disk read entirely.

## 5. The Architecture (storage.h)

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

## 6. The Implementation Guide (storage.c)

Here is how you will implement this in `src/storage.c`:

### `column_reader_create`
1. Use `snprintf` to build the `.bin` path (`part_dir/col_name.bin`).
2. Call `mmap_file_open()` on that path.
3. Calculate `total_granules`. You must use ceiling division to account for a partial last granule: `(mmap->size + bytes_per_granule - 1) / bytes_per_granule`.
4. Store the `mask` pointer. (The reader doesn't own the mask, it just borrows it from the index!)

### `column_reader_next_granule_i32`
This is the heart of the engine. It is a `while` loop that advances `current_granule_idx` until it finds a Granule where the bit in the `mask` is `1`.
1. Check if `current_granule_idx >= total_granules`. If so, return an empty slice (len = 0).
2. If `mask` is provided, check `bitmap_is_set(mask, current_granule_idx)`. 
   - If it's `0`, skip it! Increment `idx` and `continue`.
3. If it's `1` (or if `mask` is NULL for a full table scan), we've found our chunk!
4. Calculate the memory pointer: `const int32_t* ptr = (const int32_t*)mmap->data + (current_granule_idx * 8192);`
5. Calculate the remaining rows (it will be 8192, unless it's the very last granule in the file which might be partially full).
6. Increment `current_granule_idx`.
7. Return `slice_of(ptr, rows)`.

### Implementation Edge Cases (Bulletproofing the Reader)
When building the reader, watch out for these three fatal bugs:
1. **The NULL Mask (Full Table Scans)**: If a query has no `WHERE` clause, `reader->mask` will be `NULL`. You must explicitly check `if (reader->mask != NULL && !bitmap_is_set(...))` to prevent a Segmentation Fault inside the bitmap library.
2. **Pointer Dereference Trap**: When calculating the `ptr` for the slice, use pure pointer arithmetic: `(const int32_t*)mmap->data + (idx * 8192)`. Do **not** use array syntax like `&(...[idx])` incorrectly. If you accidentally dereference the array and take the address of a local stack variable, the execution engine will crash when trying to read the out-of-scope memory!
3. **The Partial Last Granule**: The final Granule in the file rarely has exactly 8,192 rows. If `current_granule_idx == total_granules - 1`, you must calculate the leftover rows using `(mmap->size / sizeof(int32_t)) - (idx * 8192)`. If you blindly yield 8,192, the engine will read OS memory garbage past the end of the file.

## 7. Testing the Reader

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

This tiny system is exactly how  analytical databases manage to scan terabytes of data without running out of RAM.


This system enables analytical databases to scan large datasets while maintaining constant memory utilization. In the next section, query execution will be paused to explore the construction of search indexes for unstructured text.