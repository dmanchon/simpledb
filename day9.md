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

### What exactly is a Granule?
A **Granule** is simply a logical block of rows—in our case, exactly 8,192 rows. 

Why 8,192? It is the perfect sweet spot for modern CPUs. If a column is composed of 32-bit integers, 8,192 rows equals exactly 32 KB of data. This perfectly matches the size of an L1 Data Cache on most modern CPUs. By chunking our data into 8,192-row granules, we guarantee that when our SIMD primitives scan a granule, it stays entirely resident in the fastest memory layer available.

### Why not just use `fseek`? 
You might be wondering: *If a column is just an array of `int32_t`, why do we need a Marks file at all? If I want row 81,920, can't I just `fseek` to `81920 * sizeof(int32_t)`?*

Yes, you can... **until you add compression.** 

In modern databases, disk I/O is the ultimate bottleneck. Soon, we will compress our `.bin` files using algorithms like LZ4 or Zstandard. Once a block of data is compressed, its physical byte size becomes completely unpredictable. You can no longer calculate a byte offset using simple math. 

The **Marks File (`.mrk`)** acts as the absolute bridge between logical row numbers and physical compressed byte offsets.

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

### Understanding the Column Writer

The `column_writer_create` function signature takes three arguments that define exactly how and where data is flushed to disk:

1. **`part_dir`**: In columnar systems, a table isn't one monolithic file. It is partitioned into immutable folders called **Parts** (e.g., `part_20260816_1_1_0/`). The `part_dir` tells the writer exactly which folder to write into.
2. **File Generation**: The writer will `fopen()` and create two distinct files inside the part directory:
   - `<part_dir>/<col_name>.bin`: The raw binary data file (where the `int32_t` arrays are flushed).
   - `<part_dir>/<col_name>.mrk`: The Marks file (where the `mark_entry_t` byte offsets are recorded at every 8,192-row boundary).
3. **`type`**: Passing the `column_type_t` is critical. Not only does it allow the writer to validate the 4-byte stride for fixed-width types (`i32`/`f32`), but if the type is `COL_TYPE_STRING_DICT`, the writer knows it must also serialize the `string_dict_t` hash map out to a third file (e.g., `<col_name>.dict`) so the strings can be decoded later!

### Abstracting the Complexity (The Two-File Dance)

You might be wondering: *If we only expose an `append` method, how do we juggle two entirely different files simultaneously?*

The beauty of the `column_writer_t` struct is that it completely abstracts the dual-file complexity away from the ingestion engine. 

1. **Inside `create()`**: The writer immediately `fopen()`s both the `<col_name>.bin` and `<col_name>.mrk` files. It also writes the very first mark entry (offset `0`) to the `.mrk` file before returning.
2. **Inside `append()`**: When you pass a chunk of data, the writer immediately calls `fwrite` to dump your data straight into the `.bin` file. It then increments its internal `rows_written` counter. Crucially, it checks if `rows_written` just crossed an 8,192-row boundary. If it did, it instantly creates a new `mark_entry_t` using its current byte offset and `fwrite`s that directly into the `.mrk` file!
3. **Inside `close()`**: It simply calls `fclose` on both pointers.

As a developer using the database, you don't even have to think about the Marks file. You just call `append()` over and over, and the writer perfectly coordinates the `.bin` and `.mrk` files for you in the background!

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

## 3. Preparation for Day 9

To build this, we'll need to write the `column_writer_append` loop carefully. 

### Chunks vs. Granules
It is vital to understand the difference between a **Chunk** and a **Granule**:
- **The Chunk**: This is the `data` array you pass into `column_writer_append`, along with its `num_rows`. The chunk size is entirely arbitrary and dictated by the caller. If your JSON ingestion engine just parsed 500 events, you pass a chunk of 500 rows. If you have 50,000 events, you pass 50,000 rows.
- **The Granule**: The database demands that Marks are generated at strict 8,192-row intervals. 

The `column_writer_append` function bridges this gap. It maintains a running counter (`rows_written`). As it streams your arbitrary "chunks" into the `.bin` file, it continuously checks if it has crossed an 8,192-row Granule boundary. Every time it does, it intercepts itself, calculates the current byte offset of the `.bin` file, and appends a new `mark_entry_t` to the `.mrk` file.

This architecture—abstracting the strict Granule boundaries away from the arbitrary ingestion chunk sizes—is the bedrock of ClickHouse's legendary read speed, and sets the stage perfectly for Sparse Primary Indexing in Day 10!
