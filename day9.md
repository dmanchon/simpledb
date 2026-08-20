# Day 9: Streaming Uncompressed Storage 

As the analytical database scales to processing larger datasets, maintaining all data in memory becomes unfeasible. In-memory column arrays must be flushed to disk to create immutable historical blocks, commonly referred to as "parts".

However, naive on-disk storage creates a problem: **File Descriptor Exhaustion**. 

If a database has 100 columns, and flushes a new chunk of data to disk every minute, it generates 100 new files per minute. Over time, the operating system limits on inodes and open files will be exceeded. 

This section resolves this by streaming data into a single `.bin` file per column.

## 1. Streaming Uncompressed Columns

Instead of writing numerous small files, data is appended to a single binary file per column (`column.bin`).

The column data is logically divided into chunks of 8,192 rows, called **Granules**. 

### The Function of a Granule
A **Granule** is a logical block of rows. In this architecture, it is exactly 8,192 rows. 

For a column composed of 32-bit integers, 8,192 rows equal 32 KB of data. This matches the size of an L1 Data Cache on most modern CPUs. Chunking data into 8,192-row granules ensures that during sequential scans, the data stays resident in the lowest-latency cache layer.

### Random Access via Mathematics
Because every row for the supported types (`COL_TYPE_I32`, `COL_TYPE_F32`, dictionary indices) is exactly 4 bytes, the exact byte offset of any row can be calculated mathematically (`row_number * sizeof(int32_t)`). A separate metadata file is not strictly necessary for lookups. 

If a query requires row 81,920, the byte offset is calculated: `81920 * 4 = 327,680`. The system seeks directly to byte 327,680 using `fseek` and reads the necessary block, skipping the prior data.

### Architectural Rationale
1. **Minimizing File Descriptors**: Streaming inserts for a column into a single `.bin` file requires only 1 open file descriptor per column.
2. **O(1) Disk Seeks**: Direct lookup times are achieved using offset calculations.
3. **Foundation for Sparse Indexing**: Day 10 covers the Sparse Primary Index. The index maps keys to granules, allowing direct memory access lookups based on those offsets.

## 2. The Data Structures

A writer utility is required to serialize the in-memory buffers to disk.

### `include/simpledb/storage.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "simpledb/allocator.h"
#include "simpledb/schema_registry.h"

// A Granule is exactly 8,192 rows.
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

### The Column Writer

The `column_writer_create` function signature takes three arguments that define how data is flushed to disk:

1. **`part_dir`**: In columnar systems, a table is partitioned into immutable folders called **Parts** (e.g., `part_20260816_1_1_0/`). The `part_dir` specifies the output directory.
2. **File Generation**: The writer calls `fopen()` to create or append to `<part_dir>/<col_name>.bin`, the raw binary data file.
3. **`type`**: The `column_type_t` validates the byte stride for fixed-width types.

The `column_writer_append` implementation streams chunks to disk:
```c
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows) {
    if (num_rows == 0) return;
    
    // Current supported types (i32, f32, dict) are all 4 bytes.
    // A fully featured engine would use a lookup table mapping type to sizeof(type).
    size_t bytes_to_write = num_rows * sizeof(int32_t);
    fwrite(data, 1, bytes_to_write, writer->bin_fp);
    
    writer->rows_written += num_rows;
}
```

## 3. Implementation Details

### Error Handling Philosophy
When implementing `column_writer_create`, consider the failure modes of `fopen`. 

If a directory doesn't exist or permissions are denied, using `assert(writer->bin_fp != NULL)` acts as a panic mechanism that terminates the process. 

For a database engine, panicking on file I/O is unacceptable. The appropriate architecture cleanly closes any partially opened files, frees memory, and returns an error. This allows the ingestion engine to log the error and fail the specific query without affecting the main database daemon.

*(Note: `assert()` is erased by the C preprocessor in Release builds with `-DNDEBUG`. To terminate in production, a custom macro calling `abort()` is required.)*

### Variable-Length Data and `.mrk` Files
Production systems like ClickHouse also write `.mrk` (Marks) files to track Granule offsets. 

The mathematical offset approach breaks when introducing:
1. **Variable-Length Strings**: If raw strings are written directly to the `.bin` file, row lengths vary.
2. **Compression**: An 8,192-row Granule of zeros compresses efficiently, while a Granule of randomized data does not. Granule byte sizes become variable.

When Granule sizes are unpredictable, mathematical offset calculation is impossible. A `.mrk` file is required to act as the map. Until variable-length strings or compression are implemented, the `.bin`-only streaming architecture is sufficient.

### String Concatenation and `snprintf`
C strings are raw pointers to byte arrays ending with a null terminator (`\0`). The `strcat(dest, src)` function copies bytes from `src` into the memory space of `dest`. If `dest` is not allocated with enough capacity, `strcat` silently overwrites adjacent memory, leading to segmentation faults or buffer overflows.

The safe standard for constructing dynamic strings is `snprintf`:
```c
char bin_path[1024]; // Allocate a fixed 1KB buffer on the stack
snprintf(bin_path, sizeof(bin_path), "%s/%s.bin", part_dir, col_name);
```
Passing `sizeof(bin_path)` guarantees the standard library will not write beyond the allocated buffer.
