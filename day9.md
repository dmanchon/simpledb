# Day 9: Streaming Uncompressed Storage (The YAGNI Pivot)

As our analytical database scales up to processing billions of rows, keeping everything purely in memory is no longer an option. We must flush our in-memory column arrays to disk to create immutable historical blocks (often called "parts").

However, naive on-disk storage creates a massive problem: **File Descriptor Exhaustion**. 

If a database has 100 columns, and we flush a new chunk of data to disk every minute, we will generate 100 new files per minute. Over a month, we'd have millions of files. The OS kernel will collapse under the weight of inode lookups and open file limits. 

Today, we will solve this by streaming all of our data into a single `.bin` file per column.

---

## 1. Streaming Uncompressed Columns

Instead of writing thousands of tiny files, we will write a single, massive binary file per column (`column.bin`).

We logically divide the column data into chunks of 8,192 rows, called **Granules**. 

### What exactly is a Granule?
A **Granule** is simply a logical block of rows—in our case, exactly 8,192 rows. 

Why 8,192? It is the perfect sweet spot for modern CPUs. If a column is composed of 32-bit integers, 8,192 rows equals exactly 32 KB of data. This perfectly matches the size of an L1 Data Cache on most modern CPUs. By chunking our data into 8,192-row granules, we guarantee that when our SIMD primitives scan a granule, it stays entirely resident in the fastest memory layer available.

### Random Access via Pure Math 
You might be wondering: *If a column is just a massive array of `int32_t` on disk, how do we skip to row 81,920?*

Because every row is exactly 4 bytes, you can calculate the exact byte offset of any row using pure math (`row_number * 4`). You don't actually need a metadata file to tell you where row 81,920 lives. 

If a query asks for row 81,920, we calculate the byte offset: `81920 * 4 = 327,680`. We jump straight to byte 327,680 using `fseek` and read exactly what we need. We skip scanning all the prior data!

### The "Why" behind the Architecture:
1. **Defeating File Descriptor Limits**: By streaming all inserts for a column into a single `.bin` file, we only need 1 open file per column, regardless of whether it has a thousand rows or a billion rows.
2. **O(1) Disk Seeks**: We achieve instant lookup times using pure math.
3. **The Foundation for Sparse Indexing**: Tomorrow (Day 10), we will build the Sparse Primary Index. The Primary Index will tell us *which granules* contain the data we want, and our pure math logic will jump directly to them.

---

## 2. The Data Structures

We need a writer utility to serialize our in-memory buffers to disk.

### `include/simpledb/storage.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

Our `column_writer_append` logic is beautifully simple:
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

## 3. Preparation for Day 9

To build this, we'll need to write the `column_writer_append` loop carefully. 

### Error Handling Philosophy: Panics vs. Graceful Bubbling
When implementing `column_writer_create`, you must make a critical architectural decision: **What happens if `fopen` fails?** 

If a directory doesn't exist or permissions are denied, your first instinct in C might be to use `assert(writer->bin_fp != NULL)`. However, `assert()` acts as a "panic"—it instantly kills the entire process. 

For a production database engine like PostgreSQL or ClickHouse, panicking on file I/O is catastrophic. If one user runs a bad `INSERT` query that triggers a file error, you **do not** want the entire database daemon to crash, dropping the connections of thousands of other users. 

Instead, the proper database architecture is to cleanly `fclose` any partially opened files, free the memory, and return an error (like `NULL`). This allows the ingestion engine to catch the error, log a `"Disk Full"` or `"Permission Denied"` message, and fail that specific query while keeping the server alive for everyone else!

*(Note: Never forget that `assert()` is completely erased by the C preprocessor in Release builds (`-DNDEBUG`). If you truly want to panic in production, you must write a custom macro that explicitly calls `abort()`!)*

### When WILL we need `.mrk` files?
If you read the ClickHouse documentation, you'll see they don't just write `.bin` files—they also write `.mrk` (Marks) files to track Granule offsets. 

Why didn't we build `.mrk` files today? Because the pure math approach permanently breaks the second we introduce:
1. **Variable-Length Strings**: If we eventually write raw strings directly to the `.bin` file instead of using dictionary encoding, row 1 might take 10 bytes and row 2 might take 500 bytes.
2. **Compression**: An 8,192-row Granule of zeros might compress to 10 bytes, while a Granule of random noise compresses to 32,000 bytes. 

The moment the physical byte size of a Granule becomes unpredictable, you can no longer use math to `fseek` into the `.bin` file. You *must* have a `.mrk` file to act as the map. 

But until we build those features, we are going to enjoy the blazing speed and simplicity of our `.bin`-only streaming architecture. (A classic YAGNI pivot!)

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
