# Day 12: Free Text Search & High Cardinality Architecture

> **Day 12 Branch**: View the exact code for this day at [dmanchon/simpledb (`day12`)](https://github.com/dmanchon/simpledb/tree/day12) ([diff from `day11`](https://github.com/dmanchon/simpledb/compare/day11...day12)).

### Implementation Status: The 1,000 LOC Milestone
As of this day, our entire SimpleDB engine (Zero-Copy MMap, Vectorized SIMD Filters, Sparse Indexing, Dictionary Encoding, and Raw Columnar Storage) fits into exactly **973 lines of C11 code** (excluding tests and blanks). This proves that columnar engines are not bloated; their performance comes from data-layout and memory access patterns, not millions of lines of code.

### Roadmap to V1: Ingestion and Queries
We are extremely close to a fully functional database. We have built the entire **Storage Engine** (MMap, Sparse Indexes, FTS) and all **Execution Primitives** (SIMD Filters, Dictionary Decoding). The final two milestones are:
1. **Day 13 (The Table Abstraction)**: Building the ingest layer that takes a raw JSON/CSV row and writes it across all columnar files simultaneously while updating the indexes.
2. **Day 14 (The Query Engine)**: Building the execution glue that takes a query, prunes Granules using the Sparse/FTS indexes, runs the SIMD filters on the remaining Granules, and reduces the results!

Welcome to Day 12! Today we are tackling **Free-Text Search (FTS)**. Traditional OLAP databases were notoriously bad at searching unstructured text (like  application logs, JSON payloads, or event messages). You typically had to export data to Elasticsearch. But modern analytical databases now embed native search capabilities.

## 1. What are ClickHouse and DuckDB doing?

### ClickHouse (Granule Granularity)
ClickHouse approaches text search primarily through **Data Skipping Indexes** (like `tokenbf_v1` - Token Bloom Filter). When it ingests text, it populates a Bitmap/Bloom Filter for each Granule. If you query `WHERE message LIKE '%timeout%'`, it maps the term to the Bitmap. 
- **The Pros**: The index is incredibly tiny. It uses almost zero RAM.
- **The Cons (Why it fails for Search)**: It ONLY helps you *skip* disk pages. Once it tells you "Granule 1 has the word", the execution engine still has to load all 8,192 rows and perform a slow, brute-force string search (`strstr`) to find the exact rows. Furthermore, you **cannot** rank results by relevance (BM25) because the Bitmap doesn't know *which* rows matched, nor *how many times* the word appeared!

### DuckDB (Row Granularity)
DuckDB handles text search via its `fts` (Full-Text Search) extension, which operates on **Row Granularity**. Instead of bitmaps, it builds a massive Inverted Index mapping terms to exact `(RowID, TermFrequency)` pairs.
- **The Pros**: You never have to do brute-force string scanning! It immediately gives you the exact Row IDs. And because it tracks Term Frequency (how many times the word appeared in that row), it can use the **BM25 algorithm** to rank the rows by relevance!
- **The Cons**: The index is massive. A row-level inverted index is often as large as the original text data itself!

## 2. What do we need to do exactly?

For `simpledb`, we are going to build a native **Data Skipping Index** (Granule Bitmaps) for full-text search. We want this to be extremely lean and hardware-efficient in C11, avoiding massive memory overhead.

Here is the exact architecture we need to build:

1. **The Tokenizer**: A function that takes a text column, splits it by whitespace and punctuation, and extracts terms.
2. **The Inverted Index Structure**: A hash map (or sorted array) where the `key` is the term (e.g., "timeout") and the `value` is a `bitmap_t` representing the Granules that contain this term.

## 3. Storage Engine Integration (How it all connects)
You might be wondering: *How do we actually store unstructured text, and how does this FTS index plug into the database we've built?*

### Storing Strings (Offsets & Data)
In Day 7, we built a `string_dict_t` to dictionary-encode strings (e.g., mapping "New York" to `1`). But for unstructured text like application logs, every row is unique. Dictionary encoding would cause our RAM to explode!
Instead, columnar databases store variable-length strings using two files:
1. **`column.strings`**: A massive, flat file containing the raw text strings concatenated back-to-back, separated by a `\0` byte.
2. **`column.bin`**: The standard array of 4-byte `int32_t` integers we already built. For strings, this integer represents the **byte offset** into the `.strings` file where that specific row's text begins!

### Connecting the FTS Index
Here is the lifecycle of a text search:
1. **The Write Path**: When `column_writer_append()` receives a raw string, it writes the string to `.strings`, writes the offset to `.bin`, and passes the raw string to the `inverted_index_add_document()` function. The FTS index tokenizes the string and updates its term bitmaps for the current Granule ID.
2. **The Search Path**: When a user runs a query, we call `inverted_index_search(..., "timeout")`. It returns a `bitmap_t` of matching Granules.
3. **The Read Path**: We pass that exact `bitmap_t` directly into `column_reader_create(..., mask)`. The reader uses `mmap` on the `.bin` file, completely skips Granules without the word "timeout", and only yields the `int32_t` offsets for the rows we care about!

## 4. The High-Cardinality String Problem
Before we upgrade our storage engine, we must understand *why* we are doing it. In Day 7, we built `COL_TYPE_STRING_DICT` (Dictionary Encoding). Why not just use that?

### The Limits of Dictionary Encoding
Dictionary encoding replaces strings with a 4-byte integer ID, storing the actual strings in an in-memory hash map. 
* **Low-Cardinality**: For columns with few unique values (e.g., `status = 'SUCCESS'`, `country = 'US'`), this is flawless. A billion rows might only have 5 unique strings, so the dictionary takes up almost zero RAM.
* **High-Cardinality**: Free-Text columns (like application logs, JSON payloads, or UUIDs) have massive cardinality—almost every single row is unique! If you insert 1 billion unique log messages into a dictionary-encoded column, the hash map will grow to hundreds of gigabytes in RAM, crashing your server with an Out-Of-Memory (OOM) error.

### How ClickHouse and DuckDB Handle It
Modern OLAP databases detect or allow users to specify when a string column is high-cardinality, falling back to a "Raw String" or "String Heap" architecture.
* **ClickHouse (`String` type)**: ClickHouse uses two physical files: `.bin` for the raw variable-length string bytes, and `.mrk` (Marks) for the integer offsets so the execution engine knows exactly where each string begins and ends.
* **DuckDB (`VARCHAR` type)**: DuckDB uses a hybrid approach. If a string is very short (under 12 bytes), DuckDB literally *inlines* the string directly into the vector array alongside a short prefix for ultra-fast comparisons. For long strings (e.g., JSON payloads or unstructured text logs), it stores them in a separate "String Heap" block and stores a pointer/offset in the main vector.

### The Drawbacks of Raw Strings
Storing raw variable-length strings using offsets (the ClickHouse approach) solves the RAM explosion, but introduces two massive performance drawbacks:
1. **CPU Cache Misses (Pointer Chasing)**: When scanning a dictionary-encoded column, the CPU screams through a contiguous array of integers in the L1 cache. With raw strings, the CPU must read the offset, then perform a memory jump to the string heap to fetch the characters. This unpredictable "pointer chasing" causes constant CPU cache misses, slowing down queries by orders of magnitude.
2. **Storage and I/O Bloat**: Because we can't replace the string with a 4-byte integer, we are writing gigabytes of raw text to disk. To scan the column, we have to pull all that text from the SSD into RAM, hitting I/O bottlenecks. (Databases mitigate this by compressing the string blocks using LZ4 or ZSTD before writing them to disk).

## 5. Implementing `COL_TYPE_STRING` in `storage.c`
To support Free-Text Search, we are going to build the ClickHouse architecture: a `.strings` file for the raw text, and a `.bin` file for the offsets.

1. **`schema_registry.h` & `storage.h`**: We have added `COL_TYPE_STRING` to the enum, `FILE* strings_fp` to `column_writer_t`, and `mmap_file_t* strings_mmap` to `column_reader_t`.
2. **`column_writer_create()`**: If `type == COL_TYPE_STRING`, use `snprintf` to create `part_dir/col_name.strings` and open it via `fopen("wb")` into `writer->strings_fp`.
3. **`column_writer_append()`**: If `type == COL_TYPE_STRING`, the `data` array will be an array of strings. You must loop through `num_rows`. Because `writer->strings_fp` automatically advances, calling `fwrite` repeatedly stacks the strings adjacently on disk:
```c
if (writer->type != COL_TYPE_STRING) {
    // Existing 4-byte logic
    fwrite(data, 1, bytes_to_write, writer->bin_fp);
} else {
    // New Raw String logic
    const char **strings = (const char **)data;
    for (size_t i = 0; i < num_rows; i++) {
        int32_t offset = (int32_t)ftell(writer->strings_fp);
        fwrite(&offset, sizeof(int32_t), 1, writer->bin_fp);
        fwrite(strings[i], 1, strlen(strings[i]) + 1, writer->strings_fp);
    }
}
```

*Wait, why is `data` cast to `const char**`? Aren't we appending one row at a time?*
No! Columnar databases are built for **vectorized batch ingestion**. The `column_writer_append` function receives batches of data (e.g., `num_rows = 8192` at a time). For integers, `data` is an array of `int32_t`. For strings, `data` is an array of string pointers (`const char**`). Even if you only want to insert a single row (`num_rows = 1`), you must pass the address of your string pointer so it conforms to the array API.

#### How the Offsets Work (Example)
If you insert the array `{"apple", "banana", "cherry"}`, the `.strings` file contains the raw bytes and `\0` terminators: `apple\0banana\0cherry\0`. 
The `.bin` file will contain the exact byte offsets where each word starts: `[0, 6, 13]`. The execution engine simply reads `13` from the `.bin` file, and instantly jumps to byte 13 in the `.strings` `mmap` to read "cherry"!

4. **`column_reader_create()`**: If `type == COL_TYPE_STRING`, map the `.strings` file into `reader->strings_mmap`.

Once you have upgraded `storage.c`, the execution engine can fetch the `int32_t` offsets using `column_reader_next_granule_i32()`, and use them to read the raw `char*` straight out of `strings_mmap->data`!

## 6. The FTS Architecture (Headers)

We need to add these definitions. Following our strict documentation rules, here is the header for our FTS module (`include/simpledb/fts.h`):

```c
// include/simpledb/fts.h

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "simpledb/slice.h"
#include "simpledb/bitmap.h"

// Basic Inverted Index mapping a string term to a bitmap of Granules (ClickHouse style)
// We use this for extreme data-skipping rather than BM25 scoring!
typedef struct {
    string_dict_t* map;    // String Dictionary to map Term -> Integer ID
    bitmap_t** postings;   // Array of pointers to bitmaps (indexed by the Dict ID)
    size_t num_terms;
    size_t capacity;
} inverted_index_t;

// 1. Initialize an inverted index
inverted_index_t* inverted_index_create(void);

// 2. Tokenize a text string and add it to the index for a specific Granule ID
void inverted_index_add_document(inverted_index_t* idx, const char* text, size_t granule_id);

// 3. Search the index for a term, returning a bitmap of Granules that contain it.
// This integrates with Day 10's 2-Tier Pruner!
bitmap_t* inverted_index_search(inverted_index_t* idx, const char* term);

// 4. Cleanup
void inverted_index_free(inverted_index_t* idx);
```

## 4. The Unit Test Mock

Here is how we verify our FTS engine works. We write a test that adds documents and queries the index.

```c
// tests/test_fts.c

#include "simpledb/fts.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

void test_inverted_index(void) {
    inverted_index_t* idx = inverted_index_create();
    
    // Simulate Granule 0
    inverted_index_add_document(idx, "database connection timeout", 0);
    inverted_index_add_document(idx, "timeout occurred", 0);
    
    // Simulate Granule 1
    inverted_index_add_document(idx, "query executed successfully", 1);
    
    // Simulate Granule 2
    inverted_index_add_document(idx, "connection reset by peer", 2);
    
    // Search for "timeout"
    bitmap_t* timeout_mask = inverted_index_search(idx, "timeout");
    assert(timeout_mask != NULL);
    assert(bitmap_is_set(timeout_mask, 0) == 1);
    assert(bitmap_is_set(timeout_mask, 1) == 0);
    assert(bitmap_is_set(timeout_mask, 2) == 0);
    
    // Search for "connection" (should be in Granule 0 and 2)
    bitmap_t* conn_mask = inverted_index_search(idx, "connection");
    assert(conn_mask != NULL);
    assert(bitmap_is_set(conn_mask, 0) == 1);
    assert(bitmap_is_set(conn_mask, 1) == 0);
    assert(bitmap_is_set(conn_mask, 2) == 1);
    
    inverted_index_free(idx);
    printf("FTS Inverted Index test passed!\n");
}
```

By keeping the inverted index tied to Granule IDs instead of Row IDs, we save  amounts of RAM, and the resulting bitmap can be directly bitwise-ANDed with the Sparse Primary Index bitmap from Day 10!

## 7. Lateral Discussion: Time Series & High Cardinality
*How do you build a Columnar DB to compete with Prometheus and solve the High Cardinality problem?*

Prometheus suffers from high-cardinality (e.g., adding a `user_id` or `trace_id` label) because of its architecture. Every unique combination of labels creates a brand new "Series", and Prometheus builds a massive Inverted Index mapping labels to Series IDs. Millions of users = millions of series = Out of Memory (OOM).

To tackle this with a columnar database (like ClickHouse or InfluxDB IOx), you completely invert the architecture:
1. **Flat Columnar Layout**: Instead of treating each label combination as a standalone stream, treat it as a flat table: `[Timestamp, Value, Metric_Name, Host, User_ID]`.
2. **Sort by Time**: Sort the table primarily by `Timestamp`. This allows your Sparse Index (Day 10) to prune 99% of the data just by looking at the time range.
3. **Drop the Inverted Index for High-Cardinality**: DO NOT index the `User_ID`. Store it as a Raw String (Day 12). Rely on the Time-based Sparse Index to narrow down the chunks, then use brute-force Vectorized SIMD to scan the remaining `User_ID`s in L1 cache at billions of rows per second.
4. **Delta-of-Delta Compression**: For the `Timestamp` and `Value` (F64) columns, use the Gorilla TSDB compression algorithm. Since metrics usually arrive at fixed intervals (e.g., every 15s), the delta between deltas is often `0`, resulting in massive compression ratios.

### Wait, isn't scanning high-cardinality strings slow?
You might ask: *If we drop the Inverted Index, wouldn't a query like `WHERE user_id = '12345'` or `GROUP BY user_id` be incredibly slow because we have to scan millions of rows of raw strings?*

Yes, you hit the exact architectural tradeoff! Here is how ClickHouse solves that massive read penalty:
1. **Physical Ordering**: The `ORDER BY (Timestamp, User_ID)` clause physically clusters users together on disk. The Sparse Index tracks the Min/Max values of `User_ID` for each Granule. If you query `user_id = '12345'`, the Sparse Index can skip 99% of the Granules because it knows exactly which chunks contain that user, without needing a massive Inverted Index!
2. **Data Skipping Indexes (Bloom Filters)**: If you need to filter on a high-cardinality column that is *not* part of the `ORDER BY` clause (like `trace_id`), ClickHouse lets you build a lightweight Bloom Filter for that specific column. It probabilistically tells the engine to skip the Granule, avoiding the full scan.
3. **Distributed SIMD**: For aggregations (`GROUP BY trace_id`), ClickHouse shards the flat table across 50+ nodes and uses SIMD hash-tables to aggregate the raw strings in parallel.

### What if we GROUP BY labels that are scattered everywhere?
If you run `SELECT p90(latency) GROUP BY trace_id` and `trace_id` is not clustered, the query is **not** inherently fast! In fact, this is the most expensive query you can run in an OLAP database. 

Because the labels are scattered all over the disk, the engine must scan everything and build a gigantic Hash Table in RAM to keep track of the aggregation state for every unique label. If it doesn't fit in RAM, it 'spills to disk', bringing performance to a crawl.

How do companies actually make dashboards fast for this scenario?
1. **Materialized Views (Rollups)**: You don't query the raw data for dashboards. You create a Materialized View (a background job in ClickHouse) that automatically pre-aggregates the data as it arrives (e.g., `GROUP BY minute, label`) and stores the result in a new, much smaller table.
2. **Intermediate States (T-Digest)**: For percentiles like p90, you can't just average pre-calculated p90s. Instead, the Materialized View stores an *Aggregation State* (like a T-Digest or HDRHistogram sketch). At query time, the engine simply merges these pre-calculated sketches together, turning a 5-minute query into a 50-millisecond query!

### What if a table has High-Cardinality columns, but we don't query them?
What if you run `SELECT p90(latency) GROUP BY endpoint`? Here, `endpoint` is low-cardinality, but the table *also* contains a massive `trace_id` column. Does the existence of `trace_id` slow down the query?

**Zero.** It has absolutely zero cost! 
Because this is a **Columnar Database**, every column is stored in isolated files on disk (e.g., `endpoint.bin`, `latency.bin`, `trace_id.strings`). 
When the execution engine plans that query, it only opens and scans the `endpoint` and `latency` files. The massive `trace_id` file is completely ignored—it never consumes Disk I/O, it never enters RAM, and it uses 0 CPU cycles. This is the exact reason why Columnar databases dominate analytical workloads over Row-oriented databases like PostgreSQL!

### What about Dynamic Maps or JSON? (e.g. `MetricAttribute['status']`)
If you store dynamic attributes as a massive raw JSON string, querying `GROUP BY MetricAttribute['status']` is incredibly slow. The engine has to load the entire JSON string into memory, parse it at runtime, extract the 'status' value, and then aggregate it.

To make this blazingly fast, Columnar databases use two tricks:
1. **Dynamic Subcolumns (JSON Flattening)**: When ClickHouse (via the `JSON` type) or DuckDB ingests a JSON payload, it dynamically flattens it. If it sees `{"status": "200"}`, it literally creates a brand new, hidden columnar file on disk called `MetricAttribute.status.bin`. If you query `MetricAttribute['status']`, it just reads that tiny, isolated column file! It runs exactly as fast as a native column.
2. **Two-Array Maps (Write-Time Parsing)**: If the keys are too dynamic to create thousands of subcolumns, databases store a `Map` as two parallel arrays: `keys.strings` and `values.strings`. To group by 'status', the engine scans the `keys` array to find the index of 'status', then jumps to the same index in the `values` array.
## Part 2: Building the FTS Tokenizer
Now that we can store raw strings, we need to build the foundation of our Free Text Search (FTS) engine: the Tokenizer.

A Tokenizer takes a raw string (e.g. `"Hello, World! 123"`) and breaks it into normalized tokens (e.g. `["hello", "world", "123"]`).

### Zero-Copy Tokenization (Performance)
The biggest mistake you can make when writing a Tokenizer is using `malloc` or `strdup` to create a copy of every single word. If you ingest 1 million log lines, you will trigger 20 million `malloc` calls, completely thrashing your CPU.

Instead, we use **Zero-Copy Tokenization**. 
Our `token_t` struct does not contain a `char*` buffer. It contains a `const char*` pointer to the start of the word inside the *original* string, and a `size_t length`. The execution engine simply scans the string, keeping track of boundaries, and yields these zero-copy slices.

### The Header (`include/simpledb/tokenizer.h`)
```c
#ifndef SIMPLEDB_TOKENIZER_H
#define SIMPLEDB_TOKENIZER_H

#include <stddef.h>

// A zero-copy slice of a string representing a single word
typedef struct {
    const char* text;
    size_t len;
} token_t;

// The stateful iterator for the Tokenizer
typedef struct {
    const char* input;
    size_t input_len;
    size_t current_pos;
} tokenizer_t;

// Initialize the tokenizer with a raw string
tokenizer_t tokenizer_create(const char* input, size_t input_len);

// Yield the next token (returns 1 if found, 0 if EOF)
int tokenizer_next(tokenizer_t* tokenizer, token_t* out_token);

#endif
```

### The Implementation (`src/tokenizer.c`)
To implement `tokenizer_next` without allocating memory, we use a two-pointer state machine. The algorithm is straightforward:
1. **Skip Punctuation**: Run a `while` loop that advances the `current_pos` as long as the character is *not* alphanumeric (using `<ctype.h>`'s `isalnum()`).
2. **Mark Start**: Once you hit an alphanumeric character, set `out_token->text` to point to that exact byte in the original string, and set `out_token->len = 0`.
3. **Measure Length**: Run a second `while` loop that advances `current_pos` (and increments `out_token->len`) as long as the character *is* alphanumeric.

### Architectural Limitations of our Tokenizer
Is tokenizing purely by `[A-Za-z0-9]` a good idea? 
For a basic engine, yes! This is equivalent to SQLite's `simple` tokenizer. However, it has two major limitations:
1. **Punctuation & Unicode**: Words like `don't` become `don` and `t`. Hyphenated words like `state-of-the-art` are split. Furthermore, C's default `isalnum()` does not understand UTF-8 multi-byte characters!
2. **Case Insensitivity**: Usually, tokenizers lowercase every word so `Hello` matches `hello`. **But we are using Zero-Copy!** Because our `token_t` points directly to the read-only `mmap` string, we cannot mutate the characters to lowercase them. We will have to handle case-insensitivity later during the Inverted Index hashing phase (using `strncasecmp`).

### Fixing Unicode and Punctuation (The "Smart" Tokenizer)
To fix the limitations above without importing a massive library like ICU, we can write a highly optimized `is_token_char` function. 
Since UTF-8 characters always have their high bit set (ASCII values >= 128), we can blindly accept all bytes `>= 128` as valid token characters! We can also explicitly whitelist hyphens and apostrophes.

```c
// src/tokenizer.c
#include "simpledb/tokenizer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

tokenizer_t tokenizer_create(const char *input, size_t input_len) {
    tokenizer_t tokenizer = {.input = input, .input_len = input_len, .current_pos = 0};
    return tokenizer;
}

static bool is_token_char(char c) {
    unsigned char uc = (unsigned char)c;
    if (isalnum(uc)) return true;
    if (uc == '\'' || uc == '-') return true; // Keep don't and state-of-the-art
    if (uc >= 128) return true; // Blindly accept all UTF-8 multi-byte chars!
    return false;
}

int tokenizer_next(tokenizer_t *tokenizer, token_t *out_token) {
    if (!tokenizer || !out_token || !tokenizer->input)
        return 0;

    while (tokenizer->current_pos < tokenizer->input_len &&
           !is_token_char(tokenizer->input[tokenizer->current_pos])) {
        tokenizer->current_pos++;
    }

    if (tokenizer->current_pos >= tokenizer->input_len) {
        return 0;
    }

    size_t start_pos = tokenizer->current_pos;

    while (tokenizer->current_pos < tokenizer->input_len &&
           is_token_char(tokenizer->input[tokenizer->current_pos])) {
        tokenizer->current_pos++;
    }

    out_token->text = tokenizer->input + start_pos;
    out_token->len = tokenizer->current_pos - start_pos;

    return 1;
}
```
## Part 3: The Data Skipping Index (Granule Bitmaps)
With our Zero-Copy Tokenizer ready, we need to build the actual Search Index.

We had to make a critical architectural decision here: **BM25 Relevance Ranking** (DuckDB) vs **Data Skipping** (ClickHouse). 
We chose the **ClickHouse approach**. 

For Time-Series databases and Application Logs, users rarely care about Google-style relevance ranking. They just want to know *"did the word 'panic' appear in this hour of logs?"*
By mapping `Term -> Bitmap of Granules`, our index is incredibly tiny (using almost zero RAM) and plugs directly into our Day 10 2-Tier Pruner! We sacrifice BM25 ranking to gain extreme memory efficiency.

### The Header (`include/simpledb/fts.h`)
```c
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "simpledb/bitmap.h"
#include "simpledb/string_dict.h"

#define FTS_INITIAL_CAPACITY 25000

// Basic Inverted Index mapping a string term to a bitmap of Granules (ClickHouse style)
typedef struct {
    bitmap_t** postings;   // Array of pointers to bitmaps (1 bitmap per term)
    string_dict_t* map;
    size_t num_terms;
    size_t capacity;
} inverted_index_t;

inverted_index_t* inverted_index_create(void);
void inverted_index_add_document(inverted_index_t* idx, const char* text, size_t granule_id);
bitmap_t* inverted_index_search(inverted_index_t* idx, const char* term);
void inverted_index_free(inverted_index_t* idx);
```

> **Student Question:** When writing the constructor, I did `malloc(sizeof(bitmap_t) * 16)` instead of `sizeof(bitmap_t*)`, but the compiler didn't throw a type error! Can we force C to warn us about this?
>
> **Answer:** No, because `malloc` returns a `void*`, which C silently and automatically converts to any pointer type (in this case, `bitmap_t**`). Furthermore, `sizeof()` just returns an integer. The compiler has no idea what you *intended* to put in that memory!
> 
> To permanently prevent this class of bugs, use the **Dereference `sizeof` Trick**. Instead of hardcoding the type, ask `sizeof` to measure the variable itself:
> ```c
> // DEREFERENCE the array pointer (*index->postings)
> // The compiler perfectly deduces the size based on the struct definition!
> index->postings = malloc(INITIAL_CAPACITY * sizeof(*(index->postings))); 
> ```

> **Student Question:** Wait, the index uses `malloc`? It's not on disk? Are we creating this from scratch every single time the database boots?
>
> **Answer:** Brilliant observation! You just discovered the concept of a **MemTable**. 
> If we kept this index permanently in RAM, the database would eventually run out of memory (OOM), and a server restart would wipe out the entire index, forcing us to re-tokenize gigabytes of logs!
> 
> In a true columnar database, this `inverted_index_t` is only used temporarily in RAM during **Ingestion** (batching). Once the ingestion batch is finished, the database **flushes** the index to disk as an `.fts` file (serializing the Terms and the Bitmaps). 
> At query time, the execution engine doesn't `malloc` anything; it simply `mmap`'s the `.fts` file and reads the Bitmaps straight off the disk! We are starting with the In-Memory version today, and we will build the disk-flusher in Day 13!

> **Student Question:** So if we don't have a BM25 Scorer, how do we actually find the exact rows and return them? Is this really what ClickHouse does?
>
> **Answer:** Yes, this is EXACTLY how ClickHouse Data Skipping Indexes work! 
> Because we only have a Bitmap of Granules, we don't know which exact rows matched. When the execution engine reads the Bitmap, it skips the empty Granules (saving massive Disk I/O). For the Granules that *did* have a `1` bit, it loads the 8,192 rows into the CPU and runs a fast C `strstr()` string-search across them to find the exact rows! We return the results unsorted (no relevance ranking).

> **Student Question:** If we use a standard array for `idx->terms` and do a brute-force scan to see if a token already exists during ingestion, isn't that going to be incredibly slow as the array grows?
>
> **Answer:** YES! You just perfectly diagnosed **Quadratic Time Complexity `O(M * N)`**. 
> If you ingest a 1 GB application log with 100,000 unique terms, that inner `for` loop will execute *billions* of times for every new row. It will completely freeze the database. This is exactly why an Inverted Index requires a **Hash Map** (like the `string_dict_t` we built on Day 6). Hashing brings the lookup time from `O(N)` down to `O(1)`, allowing ingestion to scale to millions of rows per second.

> **Student Question:** But how do we reuse `string_dict_t` while keeping our Tokenizer zero-copy? `string_dict_insert` expects a null-terminated string, meaning we have to `strndup` every single token!
>
> **Answer:** You just hit the hardest problem in C systems engineering! Because the user's ingest buffer is transient, the Dictionary *must* take memory ownership (allocate) for any brand new words. But it's a crime to allocate memory for words that already exist!
> 
> The industry standard solution is the **"Zero-Copy Lookup, Deep-Copy Insert"** pattern. However, rather than rewriting our Day 6 Dictionary to handle slice-lookups, we can use the **C Stack Allocation Trick**!
> Because most words are under 128 characters, we declare a tiny `char term[128]` array directly on the Stack. Stack allocation is virtually free (no `malloc` system calls). We `memcpy` the zero-copy slice into this stack buffer, null-terminate it, lowercase it, and feed it to `string_dict_insert`. 
> Our Dictionary then checks if the word exists. If it does, it returns instantly (zero allocations!). Only if it's a brand new word will the Dictionary internally call `strdup` to take memory ownership of it!

> **Student Question:** When we call `bitmap_create(64)` for a new word, what happens if that word appears again in Granule 10,000? Our bitmap capacity is too small!
>
> **Answer:** Incredible catch! You just found a fatal crash bug in our early architecture! 
> On Day 2, our `bitmap_set()` function was static: if you exceeded the capacity, it called `exit(EXIT_FAILURE)`. For our FTS index, we want our bitmaps to start incredibly tiny (e.g. 8 bytes) to save RAM, but they MUST be allowed to grow as the word appears in later granules! 
> We just upgraded `src/bitmap.c` to use `realloc` under the hood. Now, `bitmap_set()` will automatically detect an out-of-bounds index, double its capacity, zero out the new memory, and continue flawlessly.

## Recap & Final Implementation

Day 12 was a massive architectural leap! We built a hyper-efficient ClickHouse-style Data Skipping Index for text search.
Instead of trying to reinvent the wheel, we made the incredible realization that an Inverted Index is just a combination of two things we already built:
1. **The String Dictionary** (`string_dict_t` from Day 6) to convert words into integer IDs in `O(1)` time.
2. **The Bitmaps** (`bitmap_t` from Day 2) to track which Granules contain the word.

To glue them together, we built a zero-copy tokenizer that slices strings without `malloc`, and used a stack-buffer trick to safely lowercase the words and feed them into our Dictionary!

Here is our final, highly-optimized C implementation for the Inverted Index:

```c
// src/fts.c
#include "simpledb/fts.h"
#include "simpledb/bitmap.h"
#include "simpledb/string_dict.h"
#include "simpledb/tokenizer.h"
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

inverted_index_t *inverted_index_create(void) {
    inverted_index_t *index = malloc(sizeof(inverted_index_t));
    index->capacity = FTS_INITIAL_CAPACITY;
    index->map = string_dict_create();
    index->num_terms = 0;
    // CRITICAL: use calloc so all bitmap pointers start as NULL!
    index->postings = calloc(sizeof(*(index->postings)), FTS_INITIAL_CAPACITY);
    return index;
}

void inverted_index_add_document(inverted_index_t *idx, const char *text, size_t granule_id) {
    tokenizer_t tokenizer = tokenizer_create(text, strlen(text));
    token_t token;
    
    while (tokenizer_next(&tokenizer, &token)) {
        // Discard absurdly long terms to protect the stack
        if (token.len >= 128) continue;

        // Fast Stack allocation: copy the slice and make it null-terminated
        char term[128];
        memcpy(term, token.text, token.len);
        term[token.len] = '\0';

        // Lowercase for case-insensitive search
        for (size_t i = 0; i < token.len; i++) {
            term[i] = tolower((unsigned char)term[i]);
        }

        // Insert into the String Dictionary! (O(1) time complexity)
        int32_t slot = string_dict_insert(idx->map, term);
        
        if (idx->postings[slot] == NULL) {
            // Brand new word! Create a tiny dynamic bitmap (128 bytes)
            bitmap_t *bitmap = bitmap_create(128);
            bitmap_set(bitmap, granule_id);
            idx->postings[slot] = bitmap;
            idx->num_terms++;
        } else {
            // Word already exists! Just set the bit.
            bitmap_set(idx->postings[slot], granule_id);
        }
    }
}

bitmap_t *inverted_index_search(inverted_index_t *idx, const char *term) {
    size_t len = strlen(term);
    if (len >= 128) return NULL;

    // Stack allocate and lowercase the search term
    char term_lower[128];
    memcpy(term_lower, term, len);
    term_lower[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        term_lower[i] = tolower((unsigned char)term_lower[i]);
    }

    // O(1) Dictionary Lookup
    int32_t slot = string_dict_lookup(idx->map, term_lower);
    if (slot == STRING_DICT_NOT_FOUND) {
        return NULL;
    }

    // Return the boolean mask of Granules!
    return idx->postings[slot];
}

void inverted_index_free(inverted_index_t *idx) {
    string_dict_free(idx->map);
    // Free every allocated bitmap to prevent memory leaks
    for (size_t i = 0; i < idx->num_terms; i++) {
        bitmap_free(idx->postings[i]);
    }
    free(idx->postings);
    free(idx);
}
```

---

> **Day 12 Branch**: View the exact code for this day at [dmanchon/simpledb (`day12`)](https://github.com/dmanchon/simpledb/tree/day12) ([diff from `day11`](https://github.com/dmanchon/simpledb/compare/day11...day12)).
