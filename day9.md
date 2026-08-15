# Day 9: ClickHouse-Style On-Disk Column Layout & Marks Files (`.mrk`)

As our analytical database scales up to processing billions of rows, keeping everything purely in memory is no longer an option. We must flush our in-memory column arrays to disk to create immutable historical blocks (often called "parts").

However, naive on-disk storage creates a massive problem: **File Descriptor Exhaustion**. 

If a database has 100 columns, and we flush a new chunk of data to disk every minute, we will generate 100 new files per minute. Over a month, we'd have millions of files. The OS kernel will collapse under the weight of inode lookups and open file limits. 

Today, we will solve this by implementing the architecture pioneered by ClickHouse: **Marks Files**.

---

## 1. Granules and The Marks File Architecture

Instead of writing thousands of tiny files, we will write a single, massive binary file per column (`column.bin`), and a companion metadata file called a **Marks File** (`column.mrk`).

We logically divide the column data into chunks of 8,192 rows, called **Granules**. 
The Marks File stores exactly one entry per Granule. Each entry tells us the exact byte offset in the `column.bin` file where that Granule begins.

### Why is this brilliant?
1. **O(1) Seek Time**: If we want to read row `81920`, we know it is exactly at Granule `10` (81920 / 8192). We look up the 10th entry in the Marks File to get the byte offset, and jump straight to it in the `.bin` file!
2. **Reduced File Handles**: We only keep 2 open files per column, no matter how many millions of rows are in the part.
3. **Prepared for Compression**: Later, when we compress the `.bin` file, the byte offsets will become unpredictable. The Marks File bridges the gap, telling us exactly where the compressed blocks begin.

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
#define GRANULE_SIZE 8192

// Represents a single entry in the .mrk file
typedef struct {
    uint64_t offset_in_bin;  // The byte offset in the .bin file where this granule starts
    uint64_t num_rows;       // Usually 8192, but the final granule might be smaller
} mark_entry_t;

// State for flushing an in-memory column to disk
typedef struct {
    const char* column_name;
    column_type_t type;
    
    FILE* bin_fp;            // Open file pointer for writing the raw data
    FILE* mrk_fp;            // Open file pointer for writing the marks
    
    uint64_t current_offset; // Running total of bytes written to bin_fp
    uint64_t rows_written;   // Running total of rows written
} column_writer_t;

// Initialize a writer for a specific column in a target directory
column_writer_t* column_writer_create(const char* part_dir, const char* col_name, column_type_t type);

// Write a chunk of data to disk, generating marks at 8192-row boundaries
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows);

// Close files and free resources
void column_writer_close(column_writer_t* writer);
```

---

## 3. Preparation for Day 9

To build this, we'll need to write the `column_writer_append` loop carefully. As it streams through the incoming `data` array, it needs to check if it has crossed an 8,192-row boundary. Every time it does, it must append a new `mark_entry_t` to the `.mrk` file, recording the current byte offset.

This architecture is the bedrock of ClickHouse's legendary read speed, and sets the stage perfectly for Sparse Primary Indexing in Day 10!
