# Day 9: Streaming Uncompressed Storage (The YAGNI Pivot)

As our analytical database scales up to processing billions of rows, keeping everything purely in memory is no longer an option. We must flush our in-memory column arrays to disk to create immutable historical blocks (often called "parts").

However, naive on-disk storage creates a massive problem: **File Descriptor Exhaustion**. 

If a database has 100 columns, and we flush a new chunk of data to disk every minute, we will generate 100 new files per minute. Over a month, we'd have millions of files. The OS kernel will collapse under the weight of inode lookups and open file limits. 

Today, we will solve this by implementing the architecture pioneered by ClickHouse: **Marks Files**.

---

## 1. Granules and The Marks File Architecture

Instead of writing thousands of tiny files, we will write a single, massive binary file per column (`column.bin`), and a companion metadata file called a **Marks File** (`column.mrk`).

We logically divide the column data into chunks of 8,192 rows, called **Granules**. 
The Marks File stores exactly one entry per Granule. Each entry tells us the exact byte offset in the `column.bin` file where that Granule begins.

### What exactly is a Granule?
A **Granule** is simply a logical block of rows—in our case, exactly 8,192 rows. 

Why 8,192? It is the perfect sweet spot for modern CPUs. If a column is composed of 32-bit integers, 8,192 rows equals exactly 32 KB of data. This perfectly matches the size of an L1 Data Cache on most modern CPUs. By chunking our data into 8,192-row granules, we guarantee that when our SIMD primitives scan a granule, it stays entirely resident in the fastest memory layer available.

### Why not just use `fseek`? 
You might be wondering: *If a column is just an array of `int32_t`, why do we need a Marks file at all? If I want row 81,920, can't I just `fseek` to `81920 * sizeof(int32_t)`?*

Yes, you can... **until you add compression.** 

In modern databases, disk I/O is the ultimate bottleneck. Soon, we will compress our `.bin` files using algorithms like LZ4 or Zstandard. Once a block of data is compressed, its physical byte size becomes completely unpredictable. You can no longer calculate a byte offset using simple math. 

To solve this, modern ClickHouse Marks files actually store **two offsets** for every Granule:
1. **The Compressed Block Offset**: The absolute byte offset in the `.bin` file where the LZ4/ZSTD compressed block begins.
2. **The Decompressed Offset**: The byte offset *inside* that block where the specific Granule starts (after the block has been decompressed in memory).

The **Marks File (`.mrk`)** acts as the absolute bridge between logical row numbers and physical compressed byte offsets. When a query asks for row 100,000, the Marks file says: *"Go to byte 40,291 in the `.bin` file, decompress that block, and then skip to byte 1,024 inside the decompressed memory."*

*(Note: For Day 9 of SimpleDB, to keep our C implementation strictly focused on the core architecture and avoid bringing in massive external dependencies like LZ4, we will write our `.bin` files completely uncompressed. Our `.mrk` files will therefore only need to store a single absolute byte offset. But by building this architecture now, we leave the door perfectly open to drop in compression later without changing how the database reads!)*

### The "Why" behind the Architecture:
1. **Defeating File Descriptor Limits**: A database with 100 columns receiving inserts every minute would normally create 100 new files per minute. The OS kernel will quickly crash from inode exhaustion. By streaming all inserts for a column into a single `.bin` file, we only need 2 open files per column, regardless of whether it has a thousand rows or a billion rows.
2. **O(1) Disk Seeks**: If a query asks for row 81,920, we calculate the granule index: `81920 / 8192 = 10`. We jump to the 10th entry in the `.mrk` file, read the exact byte offset recorded there, and then `fseek` straight to that byte in the `.bin` file. We skip scanning all the prior data.
3. **The Foundation for Sparse Indexing**: Tomorrow (Day 10), we will build the Sparse Primary Index. The Primary Index will tell us *which granules* contain the data we want, and the Marks File will tell us *where* those granules live on disk.

---

## 2. The Data Structures

We need structures to represent a single mark entry, the metadata for an entire part, and a writer utility to serialize our in-memory buffers to disk.

### `include/simpledb/storage.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "simpledb/allocator.h"
#include "simpledb/schema_registry.h"

// A Granule is exactly 8,192 rows.
// We don't physically record boundaries yet, but we will use this logically for indexing.
#define GRANULE_SIZE 8192

// State for flushing an in-memory column to disk
typedef struct {
    const char* column_name;
    column_type_t type;
    
    FILE* bin_fp;            // Open file pointer for writing the raw data
    
    uint64_t rows_written;   // Running total of rows written
} column_writer_t;

// Initialize a writer for a specific column in a target directory
column_writer_t* column_writer_create(const char* part_dir, const char* col_name, column_type_t type);

// Write a chunk of data to disk
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows);

// Close files and free resources
void column_writer_close(column_writer_t* writer);
```

### Understanding the Column Writer

The `column_writer_create` function signature takes three arguments that define exactly how and where data is flushed to disk:

1. **`part_dir`**: In columnar systems, a table isn't one monolithic file. It is partitioned into immutable folders called **Parts** (e.g., `part_20260816_1_1_0/`). The `part_dir` tells the writer exactly which folder to write into.
2. **File Generation**: The writer will `fopen()` a new file inside the part directory:
   - `<part_dir>/<col_name>.bin`: The raw binary data file (where the arrays are flushed).
3. **`type`**: Passing the `column_type_t` is critical. It allows the writer to validate the 4-byte stride for fixed-width types (`i32`/`f32`/`STRING_DICT`).

### The Missing `.mrk` File (The YAGNI Pivot)

If you read the first half of this blog post, you might be wondering: *Wait, where is the `.mrk` file logic in the C headers?*

When actually implementing this in C, we hit a profound realization. **Because our columns are currently uncompressed and fixed-width (exactly 4 bytes per row), a `.mrk` file is completely redundant!**

If you want to find Granule 10 (row 81,920), you can calculate the physical byte offset instantly: `81920 * 4 = 327,680`. Generating a `.mrk` file to store a number we can calculate in one nanosecond using pure math is a classic violation of YAGNI (You Aren't Gonna Need It).

So, we pivoted! We stripped the `.mrk` file logic completely out of `column_writer_t`. 

Our `column_writer_append` logic is now beautifully simple:
```c
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows) {
    if (num_rows == 0) return;
    
    // We only support 4-byte types right now
    size_t bytes_to_write = num_rows * 4;
    fwrite(data, 1, bytes_to_write, writer->bin_fp);
    
    writer->rows_written += num_rows;
}
```
That is it. That is the entire ingestion abstraction. We simply stream chunks to disk.

---

## How the Giants Do It

This specific architecture—separating the raw compressed data from a sparse offset map—is not just an academic exercise. It is the exact blueprint used by the world's fastest OLAP databases, though they each put their own spin on it:

### 1. ClickHouse (MergeTree)
What we are building today is practically a 1:1 clone of ClickHouse's famous **MergeTree** engine. In ClickHouse, every column on disk has a `.bin` file and a `.mrk` file. By default, ClickHouse also hardcodes its `index_granularity` to 8,192 rows. When you run a query, ClickHouse scans its primary index (which lives in RAM), identifies exactly which Granules it needs, looks up the byte offsets in the `.mrk` files, and issues highly parallelized disk reads directly to those offsets in the `.bin` files. 

### 2. Apache Parquet
Parquet (the industry standard for analytical data lakes) uses the exact same conceptual architecture but packages it differently. Instead of writing separate `.bin` and `.mrk` files into a directory, Parquet stuffs everything into a single monolithic file. 
- A Parquet **Row Group** (usually ~1 million rows) is analogous to our Part Directory.
- A Parquet **Data Page** (usually ~1MB of compressed data) is analogous to our Granule.
- The Parquet **Footer Metadata** serves the exact same role as our `.mrk` file, containing the byte offsets to jump directly to specific Data Pages.

### 3. DuckDB
DuckDB is designed to be embedded, so like Parquet, it prefers single-file databases (`.duckdb`). DuckDB organizes data into **Row Groups** (122,880 rows), which are internally divided into **Vector Blocks** of 2,048 rows. The metadata containing the byte offsets for these blocks is stored in the file headers and block pointers, completely bypassing the need for an external marks file while achieving the exact same O(1) block lookup.

---

## Introspecting the Engine in SQL

Because this architecture is identical to ClickHouse, we can look at how ClickHouse actually exposes these internals to database administrators natively via SQL. You can literally look at the physical `.bin` and `.mrk` files from the SQL command line:

1. **Viewing the Parts (The Directories)**
   The `system.parts` table shows you every single immutable part directory currently sitting on the disk. It includes a `marks` column that tells you exactly how many Granules exist in that specific part directory:
   ```sql
   SELECT name, bytes_on_disk, rows, marks 
   FROM system.parts 
   WHERE table = 'my_table' AND active = 1;
   ```

2. **Viewing the `.bin` and `.mrk` Files**
   You can query `system.parts_columns` to see the exact byte sizes of the `.bin` and `.mrk` files for every single column. This is phenomenal for seeing which columns are consuming disk space and verifying compression efficiency:
   ```sql
   SELECT 
       column, 
       data_compressed_bytes, -- The size of the column.bin file
       marks_bytes            -- The size of the column.mrk file
   FROM system.parts_columns 
   WHERE table = 'my_table';
   ```

3. **Watching the Marks at Query Time**
   By prefixing any query with `EXPLAIN indexes = 1`, ClickHouse will output a highly detailed log showing exactly how many marks (Granules) existed, and how many it completely skipped reading from the `.mrk` and `.bin` files because of the primary index:
   ```sql
   EXPLAIN indexes = 1 
   SELECT count(*) FROM my_table WHERE user_id = 123;
   ```

---

## 4. Preparation for Day 9

To build this, we'll need to write the `column_writer_append` loop carefully. 

### Error Handling Philosophy: Panics vs. Graceful Bubbling
When implementing `column_writer_create`, you must make a critical architectural decision: **What happens if `fopen` fails?** 

If a directory doesn't exist or permissions are denied, your first instinct in C might be to use `assert(writer->bin_fp != NULL)`. However, `assert()` acts as a "panic"—it instantly kills the entire process. 

For a production database engine like PostgreSQL or ClickHouse, panicking on file I/O is catastrophic. If one user runs a bad `INSERT` query that triggers a file error, you **do not** want the entire database daemon to crash, dropping the connections of thousands of other users. 

Instead, the proper database architecture is to cleanly `fclose` any partially opened files, free the memory, and return an error (like `NULL`). This allows the ingestion engine to catch the error, log a `"Disk Full"` or `"Permission Denied"` message, and fail that specific query while keeping the server alive for everyone else!

*(Note: Never forget that `assert()` is completely erased by the C preprocessor in Release builds (`-DNDEBUG`). If you truly want to panic in production, you must write a custom macro that explicitly calls `abort()`!)*

### When WILL we need `.mrk` files?
If we can calculate the byte offsets using math, why did we spend half this post talking about ClickHouse's Marks files?

Because the pure math approach permanently breaks the second we introduce:
1. **Variable-Length Strings**: If we eventually write raw strings directly to the `.bin` file instead of using dictionary encoding, row 1 might take 10 bytes and row 2 might take 500 bytes.
2. **Compression**: An 8,192-row Granule of zeros might compress to 10 bytes, while a Granule of random noise compresses to 32,000 bytes. 

The moment the physical byte size of a Granule becomes unpredictable, you can no longer use math to `fseek` into the `.bin` file. You *must* have the `.mrk` file to act as the map. 

But until we build those features, we are going to enjoy the blazing speed and simplicity of our `.bin`-only streaming architecture.

### A Note on C Strings and `snprintf`
If you are coming from higher-level languages like Python or Rust, you might be tempted to build the file paths inside `create()` using string concatenation like `part_dir + col_name + ".bin"`, or by using the C standard library's `strcat()` function.

**Do not use `strcat`.** 

String handling in C is notoriously clunky because C strings are just raw pointers to byte arrays in memory ending with a null terminator (`\0`). The `strcat(dest, src)` function blindly copies bytes from `src` into the memory space of `dest`. If `dest` wasn't explicitly allocated with enough extra memory to hold the new characters, `strcat` will silently overwrite adjacent memory, leading to immediate **segmentation faults** or massive security vulnerabilities (Buffer Overflows).

Instead, the modern standard in C for safely constructing dynamic strings is `snprintf`:
```c
char bin_path[1024]; // Allocate a fixed 1KB buffer on the stack
snprintf(bin_path, sizeof(bin_path), "%s/%s.bin", part_dir, col_name);
```
`snprintf` allows you to format strings safely. By passing `sizeof(bin_path)`, you guarantee the standard library will *never* write more than 1024 bytes, completely eliminating the risk of a buffer overflow.

This perfectly safe string handling, combined with our streamlined `.bin` writing logic, sets the stage perfectly for Sparse Primary Indexing in Day 10!
