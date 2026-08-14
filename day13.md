# Day 13: Deletion Vectors

> **Day 13 Branch**: View the exact code for this day at [dmanchon/simpledb (`day13`)](https://github.com/dmanchon/simpledb/tree/day13) ([diff from `day12`](https://github.com/dmanchon/simpledb/compare/day12...day13)).

> [!TIP]
> **Long-Read Engineering Log (~650 lines) — TL;DR & Fast Navigation**
> This post is an unabridged Socratic pair-programming log capturing the architecture, bitwise arithmetic, and memory layout of Roaring-style Deletion Vectors in C11.
> - **Why Deletion Vectors:** Columnar `.bin` files are immutable on disk; rewriting gigabytes of data to shift rows after a `DELETE` destroys write throughput. Instead, we record deleted `row_id`s in a separate bitset mask.
> - **Roaring 2-Level Partitioning:** Splits 32-bit row IDs into a 16-bit high chunk key (`row_id >> 16`) and a 16-bit low offset (`row_id & 0xFFFF`), dynamically upgrading containers from sorted `uint16_t` arrays ($\le 4,096$ items) to $2^{16}$-bit bitmaps ($8\text{ KB}$).
> - **Want to go straight to the code?** [Jump to the C Header (`deletion_vector.h`)](#1-the-c-header-deletion_vectorh), [Jump to the Unit Tests](#5-unit-tests), or [Jump to the Full C11 Implementation (`src/deletion_vector.c`)](#6-full-implementation-deletion_vectorc).


### Context
Up until now, we've built all the critical pieces of our immutable columnar storage engine:
*   **Storage & Memory**: Zero-Copy MMap, Typed Slices.
*   **Columns**: Raw Columnar Storage, Null Bitmaps, String Dictionaries.
*   **Indexes**: Sparse Primary Indexes (Granules) and Free-Text Search.

But what happens when a user wants to run a `DELETE` operation? 

Because columnar storage relies on tightly packed, contiguous arrays (like our memory-mapped `.bin` files), deleting a row in the middle of a file would require rewriting the entire file just to shift all subsequent rows up by one position. This is extremely expensive and kills write performance.

### The Solution: Deletion Vectors
Instead of physically removing the row, modern OLAP databases (like ClickHouse, Delta Lake, and DuckDB) use **Deletion Vectors**. 
A Deletion Vector is essentially a bitmask where the n-th bit is set to `1` if the n-th row has been deleted.

During query execution, our SIMD filters will perform a bitwise `AND NOT` against the deletion vector, transparently filtering out the deleted rows without ever needing to rewrite the immutable data files. Over time, when too many rows are marked as deleted, a background compaction process will rewrite the file.

### Roaring Bitsets
Why not just use a standard uncompressed bitmap (like our `null_bitmap_t`)?
Deletions are often highly clustered (e.g., deleting an entire week of data) or highly sparse (deleting 5 specific rows out of 10 million). A standard bitmap would waste a lot of memory. 

To solve this, we use **Roaring Bitmaps**, which dynamically switch their internal representation based on the data:
1. **Array Container**: For highly sparse deletions (just stores an array of integers representing the deleted row IDs).
2. **Bitmap Container**: For dense, scattered deletions (a standard bitmask).
3. **Run Container**: For contiguous ranges (e.g., rows 1,000,000 through 2,000,000 were deleted).

### 1. The C Header (`deletion_vector.h`)
We will use a 2-level Roaring structure. A row ID (32-bit integer) is split into two halves:
- The **High 16 bits** act as the "Key" to find the chunk container.
- The **Low 16 bits** act as the "Value" stored inside that container.

```c
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DV_INITIAL_CAPACITY 16

// Roaring Container Types
typedef enum {
    CONTAINER_ARRAY,  // Stored as an array of uint16_t (for < 4096 deletes)
    CONTAINER_BITMAP, // Stored as a flat 8KB bitmap (for >= 4096 deletes)
    CONTAINER_RUN     // Stored as start/length pairs (for contiguous deletes)
} container_type_t;

typedef struct {
    container_type_t type;
    uint16_t cardinality;
    uint32_t capacity;
    void *data; // Pointer to the underlying array, bitmap, or run data
} roaring_container_t;

// The Deletion Vector abstraction
typedef struct {
    uint32_t num_rows;
    uint32_t deleted_count;
    
    // Map of high-16 bit keys to their respective containers
    uint16_t *keys; 
    roaring_container_t **containers;
    
    uint32_t num_containers;
    uint32_t capacity;
} deletion_vector_t;

// --- Lifecycle ---
deletion_vector_t* deletion_vector_create(uint32_t initial_capacity);
void deletion_vector_free(deletion_vector_t *dv);

// --- Mutations ---
void deletion_vector_add(deletion_vector_t *dv, uint32_t row_id);

// --- Queries ---
// Returns the total number of deleted rows (O(1) lookup)
uint64_t deletion_vector_get_cardinality(const deletion_vector_t *dv);

bool deletion_vector_is_deleted(const deletion_vector_t *dv, uint32_t row_id);

// Returns a flat 64-bit bitmask of deleted rows starting at `start_row`.
// Used by SIMD execution pipelines (AND NOT) to filter vector blocks!
uint64_t deletion_vector_get_simd_mask(const deletion_vector_t *dv, uint32_t start_row);
```

### 2. Deep Dive: The Three Container Types

You might be wondering: *Why exactly three container types, and why is the threshold exactly 4096?* This is the mathematical genius behind Roaring Bitmaps. Every container represents a 16-bit universe (it can hold up to 2^16 = 65,536 distinct integers).

1. **`CONTAINER_ARRAY` (Sparse Data)**
   - **How it works**: Stores deleted row IDs as a simple, sorted array of 16-bit integers (`uint16_t`).
   - **The Math**: Each integer takes 2 bytes. If you have 4,000 deleted rows, the array takes 4,000 * 2 = 8,000 bytes.
   - **Why 4096?**: 4,096 * 2 bytes = 8,192 bytes (8KB). Once you hit 4,096 elements, the array size exactly matches the size of a raw bitmap. Beyond 4096, an array is *worse* for memory than a bitmap!

2. **`CONTAINER_BITMAP` (Dense Data)**
   - **How it works**: Stores a flat bitmask where each bit represents one of the 65,536 possible integers. 
   - **The Math**: 65,536 bits divided by 8 bits/byte = exactly **8,192 bytes (8KB)**.
   - **Why use it?**: If you have 30,000 deleted rows, an Array would take 60KB. But a Bitmap *always* takes exactly 8KB. Plus, lookups become O(1) bitwise operations instead of O(log N) binary searches.

3. **`CONTAINER_RUN` (Contiguous Data)**
   - **How it works**: Uses Run-Length Encoding (RLE). Stores pairs of `[start_value, length]`.
   - **Why use it?**: Imagine a user runs `DELETE FROM table WHERE date < '2025-01-01'`, which deletes 50,000 contiguous rows. 
     - An Array would take 100KB. 
     - A Bitmap would take 8KB. 
     - A Run Container just stores `[0, 50000]`—which takes exactly **4 bytes**! It provides massive compression for bulk sequential deletes.

### 2.1 The Polymorphic Container Design (C Implementation)

To implement these containers dynamically in C, we use a struct with a polymorphic `void *data` pointer. This allows the exact same container to completely change its underlying memory footprint on the fly:

```c
typedef struct {
    container_type_t type;  // E.g., CONTAINER_ARRAY or CONTAINER_BITMAP
    uint32_t cardinality;   // Number of deleted rows in this 64K chunk
    uint32_t capacity;      // Track allocated array slots (used only in ARRAY mode)
    void *data;             // Polymorphic memory pointer
} roaring_container_t;
```

During execution, the database engine checks the `type` and casts the `void*` pointer to the correct memory shape:

**When Sparse (Array Mode):**
```c
// Cast to an array of 16-bit integers
uint16_t *array = (uint16_t *)container->data;
uint16_t value = array[index]; 
```

**When Dense (Bitmap Mode):**
```c
// Cast to a flat byte-array (8192 bytes total)
uint8_t *bitmap = (uint8_t *)container->data;

// Bitwise lookup: Find the byte, then check the specific bit
uint8_t byte_target = bitmap[low_val / 8];
bool is_deleted = (byte_target & (1 << (low_val % 8))) != 0;
```
This polymorphic casting allows us to swap the memory allocator under the hood during the Array-to-Bitmap upgrade!

### 2.2 Parallel Arrays (Keys and Containers)

Notice that `deletion_vector_t` does not use a hash map, nor does it store an array of structs like `{uint16_t key, roaring_container_t* ptr}`. Instead, it uses **Parallel Arrays**:
- `uint16_t *keys;`
- `roaring_container_t **containers;`

This Structure-of-Arrays (SoA) layout guarantees that the `keys` array is densely packed in memory (just 2 bytes per key), making binary search incredibly CPU cache-friendly. 

However, it means that during an insertion, **both arrays must be shifted in perfect lockstep**. If you insert a new key at index `2`, you must use `memmove` on *both* the `keys` array and the `containers` array to ensure `keys[i]` always maps precisely to the exact same index in `containers[i]`.

### 2.3 Why use Arrays instead of Trees? (Cache Locality)

Academically, inserting a value into a sorted array requires an $O(N)$ memory shift, whereas inserting into a Binary Search Tree (BST) is $O(\log N)$. So why does this architecture use a flat array with `memmove`?

The answer is **CPU Cache Locality**. A sparse container caps out at exactly 4,096 `uint16_t` elements (8 Kilobytes). This entire structure fits effortlessly into the CPU's ultra-fast L1 cache. When we call `memmove` on an 8KB array, the C standard library (`libc`) uses heavily optimized SIMD vector instructions to shift the memory in nanoseconds. 

If we used a Tree, every node would require left/right pointers (massive memory overhead), and traversing it would cause random memory jumps, resulting in catastrophic L2/L3 cache misses. In modern systems engineering, flat arrays with $O(N)$ `memmove` shifts consistently beat $O(\log N)$ Trees for small, bounded datasets strictly due to hardware mechanics!

### 2.4 Why Not Use `bitmap_t` for the Bitmap Container?

If we already built a `bitmap_t` abstraction (in Day 7), why doesn't `CONTAINER_BITMAP` just use that? 

1. **No Dynamic Growth**: `bitmap_t` tracks dynamic `capacity` to grow. A Roaring Bitmap container represents a fixed 64K chunk. Its bitmap is *always* exactly 65,536 bits (8,192 bytes). It never grows or shrinks, making capacity metadata redundant.
2. **Pointer Indirection**: If `container->data` pointed to a `bitmap_t` struct, which in turn contained a `uint8_t *bits` pointer, we would suffer a double-pointer hop (pointer chasing) to read a single bit. Roaring Bitmaps point `data` *directly* to the raw memory to maximize L1 cache hits.
3. **64-bit Word Size**: `bitmap_t` uses `uint8_t`. We explicitly want a flat `uint64_t` array so we can use blazing-fast 64-bit bitwise instructions (`__builtin_popcountll`) during query execution.

### 2.5 The Bitwise Math (Setting a Bit in a 64-bit Array)

When upgrading to a flat `uint64_t` bitmask, how do we set the bit for a specific `val` (e.g., `val = 130`)?

1. **The Bucket (`val / 64`)**: Because each integer in our array holds exactly 64 bits, we divide the value by 64 to find which integer "bucket" it belongs to. For `130`, `130 / 64 = 2`. It belongs in `bitmap[2]`.
2. **The Offset (`val % 64`)**: To find the exact bit inside that bucket, we use the modulo operator. `130 % 64 = 2`. We need to set the 2nd bit in the bucket.
3. **The Mask (`1ULL << offset`)**: We take a 64-bit literal `1` (`1ULL`) and shift it left by the offset. This creates a mask of all zeros except for a single `1` at the target position.
4. **Bitwise OR (`|=`)**: We merge the mask into the bucket using a bitwise OR. This flips our target bit to `1` without clearing any other bits that were already set!

### 2.6 C Syntax Spotlight: Self-Documenting Pointers

In C, a pointer like `uint64_t *out_mask` could represent a dynamic array of 1,000 elements, or it could represent a single scalar output parameter. C provides no structural distinction between the two. However, you can write self-documenting function signatures to clarify your intent:

```c
// Standard pointer (ambiguous to humans)
void get_mask(uint64_t *out_mask); 

// Self-documenting (Decays to a pointer, but tells humans to expect exactly 1 element)
void get_mask(uint64_t out_mask[1]); 

// C99 Wizardry (Tells the compiler the pointer MUST be non-null and have at least 1 element)
void get_mask(uint64_t out_mask[static 1]);
```

While `out_mask[1]` is a fantastic trick for readability, returning a 64-bit integer by value (`uint64_t get_mask()`) is generally preferred for scalars because the value never leaves the CPU register!

### 3. Algorithmic Background & Pseudocode

Roaring Bitmaps are an industry-standard compressed bitset structure used heavily by modern engines like Apache Lucene, ClickHouse, and Druid. You can explore the official specification at [roaringbitmap.org](https://roaringbitmap.org/).

The core algorithmic operation is the dynamic insertion logic that seamlessly upgrades sparse arrays into dense bitmaps when they cross the cardinality threshold (4096 elements):

```text
function add(row_id):
    high_16 = row_id >> 16
    low_16 = row_id & 0xFFFF
    
    // 1. Lower-Bound Search (returns exact insertion index if missing)
    insert_idx = lower_bound_search(keys, high_16)
    
    // 2. Check if container exists
    if insert_idx < num_containers AND keys[insert_idx] == high_16:
        container = containers[insert_idx]
    else:
        // 3. Container Allocation & Insertion
        if num_containers == capacity:
            reallocate(keys, capacity * 2)
            reallocate(containers, capacity * 2)
        
        // Shift elements right to make room (memmove in C)
        shift_right(keys, from_index=insert_idx)
        shift_right(containers, from_index=insert_idx)
        
        // Create and track new Array container
        container = create_array_container()
        keys[insert_idx] = high_16
        containers[insert_idx] = container
        num_containers++
        
    // 4. Insert value into container
    if container.type == ARRAY:
        if container.cardinality < 4096:
            insert_sorted(container.array, low_16)
        else:
            // Upgrade Threshold Reached!
            bitmap = convert_array_to_bitmap(container.array)
            bitmap.set_bit(low_16)
            replace_container(container, bitmap)
            
    else if container.type == BITMAP:
        container.bitmap.set_bit(low_16)
```

### 4. Implementation Guide (What Each Method Does)

- **`deletion_vector_create`**: Initializes the top-level structure. Naturally, `num_containers` starts at `0`. The `capacity` represents how many slots are allocated for the `keys` and `containers` arrays. Pre-allocating a small capacity (e.g., 4 or 8) is highly recommended to avoid `realloc` overhead on the very first few deletions. **Crucially, production database engines must always check for `malloc` failures here to prevent segmentation faults during Out-Of-Memory (OOM) scenarios.**
- **`deletion_vector_free`**: Because we use an Array of Pointers (`roaring_container_t **`), freeing memory requires destroying from the inside out. A classic C mistake is assuming `free(dv->containers)` frees everything. It does not! `dv->containers` is just an array of memory addresses (like an address book). Freeing it only destroys the address book, leaving the actual container structs leaked on the heap. You must: 1) Loop through all containers and `free(container->data)`, 2) Free the container struct itself: `free(container)`, 3) Free the pointer arrays: `free(dv->containers)` and `free(dv->keys)`, 4) Free the top-level struct: `free(dv)`.


- **`deletion_vector_add`**: This is the core ingestion logic for `DELETE` statements. Implementing this requires a strict 5-step C algorithm:
  1. **Bitwise Split**: Split the 32-bit `row_id` into a `high_key` (`row_id >> 16`) and `low_val` (`row_id & 0xFFFF`).
  2. **Container Routing**: Search the `dv->keys` array for `high_key`. While a production engine uses Binary Search ($O(\log N)$) to handle thousands of chunks quickly, beware of the C standard library! You **cannot** easily use `bsearch()` from `<stdlib.h>` here because it returns `NULL` on a miss. We specifically require a "Lower Bound" binary search so that if the key is missing, the algorithm returns the exact index where the key *should* be inserted to maintain sorted order. For now, a simple Linear Search ($O(N)$) is a perfectly acceptable starting point.
  3. **Container Allocation (If Missing)**: 
     - If `dv->num_containers == dv->capacity`, `realloc` both `keys` and `containers` arrays to double their capacity. Check for OOM!
     - Use `memmove` (NOT `memcpy`) to safely shift all existing keys and pointers from the insertion index exactly one slot to the right. *Note: `memcpy` causes undefined behavior when source and destination buffers overlap, which is guaranteed to happen when shifting an array in-place!*
     - Insert `high_key` into `keys`.
     - Allocate a new `roaring_container_t` (type: `CONTAINER_ARRAY`, cardinality: 0) and dynamically allocate a small `uint16_t` array for its data.
     - Increment `dv->num_containers`.
  4. **Container Insertion**: 
     - **If `CONTAINER_ARRAY`**: Binary search the array for `low_val` (idempotency check). If missing, use `memmove` to shift elements and insert `low_val` to preserve sorted order, then increment cardinality. 
     - *The Upgrade Phase*: If the array cardinality hits `4096`, allocate an 8KB flat bitmask using `calloc(1024, sizeof(uint64_t))`. Loop over the old `uint16_t` array, setting the corresponding bits in the new bitmask (`bitmap[val / 64] |= (1ULL << (val % 64))`). Free the old array, assign `container->data = bitmap`, and flip the type to `CONTAINER_BITMAP`.
     - **If `CONTAINER_BITMAP`**: Calculate the word index (`low_val / 64`) and bit offset (`low_val % 64`). Check if set. If not, set it (`bitmap[word] |= (1ULL << offset)`) and increment cardinality.
  5. **Global Tracking**: If a new deletion was successfully written (not a duplicate), increment `dv->deleted_count`.

- **`deletion_vector_get_cardinality`**: Imitating the official `roaring_bitmap_get_cardinality` API, this returns the total number of deleted rows. Because we maintain a running `deleted_count` at the struct level, this is an O(1) operation. It is extremely useful for the query optimizer to quickly determine what percentage of a table is deleted (e.g., to decide if a file rewrite compaction is necessary).

- **`deletion_vector_is_deleted`**: A simple scalar point-lookup. Used mainly when evaluating a single row, or doing slow-path execution. It routes to the correct container and checks the presence of the lower 16 bits.

- **`deletion_vector_get_simd_mask`**: This is the **Execution Engine Bridge**. Our SIMD `gather` and `filter` operations process data in 64-row batches (a 64-bit mask). This method looks at a `start_row` (which the engine guarantees is a multiple of 64), extracts exactly 64 bits worth of deletion statuses, and outputs a single `uint64_t`. 
  - *Example*: If `start_row = 64`, the function checks rows 64 through 127. If row 65 and row 67 are deleted, it returns a 64-bit integer where the 1st and 3rd bits are set to `1` (binary: `...00001010`).
  - *Optimization*: Because `start_row` is aligned to a 64-row boundary, all 64 rows are mathematically guaranteed to live in the exact same container! You only need to route to the container once. Furthermore, if it is a `CONTAINER_BITMAP`, you can return the `uint64_t` directly from the bitmap array (`bitmap[low_key / 64]`) in a single $O(1)$ memory read. No loops required!
  - If it falls back to a `CONTAINER_ARRAY`, avoid looping 64 times and re-searching the array. Instead, iterate directly over the sparse array's `cardinality` to quickly extract the matching bits:
    ```c
    uint64_t out_mask = 0;
    uint16_t *array = (uint16_t *)container->data;
    for (size_t i = 0; i < container->cardinality; i++) {
        if (array[i] >= low_key && array[i] < low_key + 64) {
            out_mask |= (1ULL << (array[i] - low_key)); // Set relative bit
        } else if (array[i] >= low_key + 64) {
            break; // Array is sorted, we can safely abort!
        }
    }
    return out_mask;
    ```
  - The execution engine uses `_mm512_kandn` to perform a bitwise `AND NOT` with this mask, instantly ignoring all deleted rows in a single hardware cycle without a single branch.

### 5. Unit Tests

Here is the testing suite you need to implement to verify the behavior of the containers and SIMD mask generation:

```c
#include "deletion_vector.h"
#include <assert.h>
#include <stdio.h>

void test_deletion_vector_sparse() {
    deletion_vector_t *dv = deletion_vector_create(10);
    
    // Add sparse deletions across completely different 64K containers
    deletion_vector_add(dv, 5);
    deletion_vector_add(dv, 1000000);
    
    assert(deletion_vector_is_deleted(dv, 5) == true);
    assert(deletion_vector_is_deleted(dv, 1000000) == true);
    assert(deletion_vector_is_deleted(dv, 6) == false);
    
    deletion_vector_free(dv);
    printf("Sparse test passed!\\n");
}

void test_deletion_vector_dense_upgrade() {
    deletion_vector_t *dv = deletion_vector_create(10);
    
    // Add 5000 deletions in the same 64K chunk to force Array -> Bitmap upgrade
    for (uint32_t i = 0; i < 5000; i++) {
        deletion_vector_add(dv, i);
    }
    
    assert(deletion_vector_is_deleted(dv, 4999) == true);
    assert(deletion_vector_is_deleted(dv, 5000) == false);
    
    deletion_vector_free(dv);
    printf("Dense upgrade test passed!\\n");
}

void test_deletion_vector_simd_mask() {
    deletion_vector_t *dv = deletion_vector_create(10);
    
    // Delete rows 2, 3, and 63
    deletion_vector_add(dv, 2);
    deletion_vector_add(dv, 3);
    deletion_vector_add(dv, 63);
    
    uint64_t mask = deletion_vector_get_simd_mask(dv, 0);
    
    // Bit 2, 3, and 63 should be 1
    assert((mask & (1ULL << 2)) != 0);
    assert((mask & (1ULL << 3)) != 0);
    assert((mask & (1ULL << 63)) != 0);
    
    // Bit 4 should be 0
    assert((mask & (1ULL << 4)) == 0);
    
    deletion_vector_free(dv);
    printf("SIMD mask export test passed!\\n");
}

int main() {
    test_deletion_vector_sparse();
    test_deletion_vector_dense_upgrade();
    test_deletion_vector_simd_mask();
    return 0;
}
```

### 6. Build vs. Import (The CRoaring Library)

When building a real-world analytical database, you face a classic engineering decision: Do we implement Roaring Bitmaps from scratch, or do we import the official [CRoaring](https://github.com/RoaringBitmap/CRoaring) C library?

**The Case for Importing `CRoaring`:**
The official library is battle-tested and highly optimized. It uses advanced AVX2 and AVX-512 SIMD instructions to perform lightning-fast unions and intersections across bitmaps. For production systems (like ClickHouse or Apache Doris), dropping in the `roaring.c` and `roaring.h` amalgamation files is a no-brainer.

**The Case for Building a Minimal Version:**
Implementing a *full* Roaring Bitmap specification with SIMD optimizations and run-length encoding is incredibly complex. However, implementing a *minimal* version (supporting only `ARRAY` and `BITMAP` containers specifically for deletions) is surprisingly easy—taking only about 150-200 lines of C code. For an educational database engine, building it from scratch teaches you exactly how memory layout, `memmove` array shifts, and bitwise logic drive modern database performance.

### 7. Memory Safety & Fil-C Compatibility

When building a C11 database engine heavily reliant on pointer arithmetic (like our Roaring Bitmap containers) and zero-copy `mmap`, memory safety is a critical concern. 

Can we compile `simpledb` using a memory-safe C compiler like [Fil-C](https://fil-c.org)?

From a build-system perspective, it is a trivial drop-in compiler swap (e.g., `CC=filc`). However, because of our specific OLAP architecture, there are three areas where Fil-C's strict provenance and bounds-tracking will rigorously test our implementation:
1. **`mmap` Zero-Copy Bounds**: Fil-C excels at tracking `malloc`/`free`. When we `mmap` our `.bin` columnar files and cast raw `void*` pointers to `uint64_t*` arrays, we must ensure our access patterns never speculatively read past the mapped page boundary.
2. **SIMD Intrinsics**: Our AVX-512 `gather` and `load` operations read memory in massive 64-byte chunks. Fil-C will strictly enforce that these wide vector loads never cross out of their allocated bounds.
3. **Container Pointer Math**: In our `deletion_vector_t`, we do heavy pointer math to cast `void *data` to `uint16_t*` (Array) or `uint8_t*` (Bitmap). Fil-C will guarantee we never experience Use-After-Free (UAF) or out-of-bounds writes during the dynamic Array-to-Bitmap upgrade!

Using Fil-C as a strict compile-time target in our CI pipeline (alongside AddressSanitizer) is a phenomenal way to guarantee memory safety without sacrificing our raw C11 execution speed.

### 8. Architectural Naming: Deletion Vector vs. Roaring Bitmap

A common architectural question when building this component is: *Should this file be named `deletion_vector.h` or `roaring_bitmap.h`?*

The answer comes down to the **Separation of Concerns**:
1. **`roaring_bitmap.h` (The Data Structure)**: This should be a purely generic, domain-agnostic library. It only knows about adding integers, checking cardinality, and safely upgrading containers. It has no concept of databases, rows, or SIMD masks.
2. **`deletion_vector.h` (The Database Component)**: This is the database-specific wrapper. It *includes* the roaring bitmap, but adds OLAP-specific context (like `get_simd_mask()` to feed 64-row blocks into our AVX-512 execution pipeline, or handling Transaction IDs for MVCC).

For our educational engine, we combined the data structure and the database logic into `deletion_vector.h` for simplicity. However, in a production engine like DuckDB or ClickHouse, the generic Roaring data structure is strictly separated from the database's Deletion Vector business logic.

### 6. Full Implementation (`deletion_vector.c`)

Because we have completed the final architecture of the Table Abstraction, here is the complete, fully-optimized C implementation for reference:

```c
#include "simpledb/deletion_vector.h"
#include "simpledb/bitmap.h"
#include "simpledb/storage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

deletion_vector_t *deletion_vector_create(uint32_t initial_capacity) {
    deletion_vector_t *dv = malloc(sizeof(*dv));
    if (!dv) {
        fprintf(
            stderr,
            "[FATAL ERROR] deletion_vector_create failed to allocate deletion_vector_t struct\n");
        exit(EXIT_FAILURE);
    }
    dv->capacity = initial_capacity;
    dv->num_containers = 0;
    dv->deleted_count = 0;
    dv->num_rows = 0;

    dv->containers = malloc(sizeof(*dv->containers) * initial_capacity);
    if (!dv->containers) {
        fprintf(stderr, "[FATAL ERROR] containers failed to allocate roaring_container_t struct\n");
        exit(EXIT_FAILURE);
    }

    dv->keys = malloc(sizeof(*dv->keys) * initial_capacity);
    if (!dv->keys) {
        fprintf(stderr, "[FATAL ERROR] keys failed to allocate int16_t struct\n");
        exit(EXIT_FAILURE);
    }

    return dv;
}

void deletion_vector_add(deletion_vector_t *dv, uint32_t row_id) {
    uint16_t high_key = row_id >> 16;
    uint16_t low_key = row_id & 0xFFFF;

    // find the container
    roaring_container_t *container = NULL;
    size_t insert_idx = 0;
    for (; insert_idx < dv->num_containers; insert_idx++) {
        if (dv->keys[insert_idx] == high_key) {
            container = dv->containers[insert_idx];
            break;
        } else if (dv->keys[insert_idx] > high_key)
            break;
    }

    // or add a new one
    if (!container) {
        // check if we need to reallocate
        if (dv->num_containers >= dv->capacity) {
            // double it!
            dv->capacity *= 2;
            void *temp;
            temp = realloc(dv->containers, sizeof(*dv->containers) * dv->capacity);
            if (!temp) {
                fprintf(stderr,
                        "[FATAL ERROR] containers failed to allocate roaring_container_t struct\n");
                exit(EXIT_FAILURE);
            }
            dv->containers = temp;

            temp = realloc(dv->keys, sizeof(*dv->keys) * dv->capacity);
            if (!temp) {
                fprintf(stderr, "[FATAL ERROR] keys failed to allocate int16_t struct\n");
                exit(EXIT_FAILURE);
            }
            dv->keys = temp;
        }

        // now we insert the container
        size_t n = dv->num_containers - insert_idx;
        memmove(&dv->keys[insert_idx + 1], &dv->keys[insert_idx], n * sizeof(*dv->keys));
        memmove(&dv->containers[insert_idx + 1], &dv->containers[insert_idx],
                n * sizeof(*dv->containers));

        dv->keys[insert_idx] = high_key;
        dv->containers[insert_idx] = malloc(sizeof(roaring_container_t));

        if (!dv->containers[insert_idx]) {
            fprintf(stderr,
                    "[FATAL ERROR] container failed to allocate roaring_container_t struct\n");
            exit(EXIT_FAILURE);
        }

        const uint32_t capacity = 16;
        dv->num_containers++;
        dv->containers[insert_idx]->cardinality = 0;
        dv->containers[insert_idx]->type = CONTAINER_ARRAY;
        dv->containers[insert_idx]->capacity = capacity;
        dv->containers[insert_idx]->data = malloc(sizeof(uint16_t) * capacity);
        if (!dv->containers[insert_idx]->data) {
            fprintf(stderr, "[FATAL ERROR] container->data failed to allocate\n");
            exit(EXIT_FAILURE);
        }

        // now container finally exists
        container = dv->containers[insert_idx];
    }

    if (container->type == CONTAINER_ARRAY) {
        if (container->cardinality < 4096) {
            // add the element
            uint16_t *array = (uint16_t *)container->data;
            size_t insert_idx = 0;
            bool found = false;
            for (; insert_idx < container->cardinality; insert_idx++) {
                if (array[insert_idx] == low_key) {
                    // already there, nothing to do
                    found = true;
                    break;
                } else if (array[insert_idx] > low_key) {
                    // shortcircuit
                    break;
                };
            }

            if (!found) {
                // insert
                // do we need to grow?
                if (container->capacity <= container->cardinality) {
                    // double size
                    container->capacity *= 2;
                    void *temp = realloc(array, sizeof(uint16_t) * container->capacity);
                    if (!temp) {
                        fprintf(stderr, "[FATAL ERROR] container->data failed to allocate\n");
                        exit(EXIT_FAILURE);
                    }
                    container->data = temp;
                    // this probably can be cleaner
                    array = temp;
                }

                long num_bytes = (container->cardinality - insert_idx) * sizeof(uint16_t);
                memmove(&array[insert_idx + 1], &array[insert_idx], num_bytes);
                array[insert_idx] = low_key;
                container->cardinality++;
                dv->deleted_count++;
            }

        } else {
            // promote to bitmap
            uint64_t *bitmap = calloc(1024, sizeof(uint64_t));
            uint16_t *array = (uint16_t *)container->data;

            for (size_t i = 0; i < container->cardinality; i++) {
                uint16_t val = array[i];
                bitmap[val / 64] |= (1ULL << (val % 64));
            }
            free(array);
            container->data = bitmap;
            container->type = CONTAINER_BITMAP;
        }
    }

    if (container->type == CONTAINER_BITMAP) {
        uint64_t *bitmap = (uint64_t *)container->data;
        uint64_t mask = (1ULL << (low_key % 64));
        if ((bitmap[low_key / 64] & mask) == 0) {
            bitmap[low_key / 64] |= mask;
            container->cardinality++;
            dv->deleted_count++;
        }
    }

    if (container->type == CONTAINER_RUN) {
        fprintf(stderr, "[FATAL ERROR] container->type=%d not supported\n", container->type);
        exit(EXIT_FAILURE);
    }
}

uint64_t deletion_vector_get_cardinality(const deletion_vector_t *dv) { return dv->deleted_count; }

void deletion_vector_free(deletion_vector_t *dv) {
    for (size_t i = 0; i < dv->num_containers; i++) {
        free(dv->containers[i]->data);
        free(dv->containers[i]);
    }
    free(dv->containers);
    free(dv->keys);
    free(dv);
}

bool deletion_vector_is_deleted(const deletion_vector_t *dv, uint32_t row_id) {
    uint16_t high_key = row_id >> 16;
    uint16_t low_key = row_id & 0xFFFF;

    // find the container
    roaring_container_t *container = NULL;
    size_t insert_idx = 0;
    for (; insert_idx < dv->num_containers; insert_idx++) {
        if (dv->keys[insert_idx] == high_key) {
            container = dv->containers[insert_idx];
            break;
        } else if (dv->keys[insert_idx] > high_key)
            break;
    }

    if (!container)
        return false;

    if (container->type == CONTAINER_ARRAY) {
        uint16_t *array = (uint16_t *)container->data;
        for (size_t i = 0; i < container->cardinality; i++) {
            if (array[i] == low_key) {
                return true;
            } else if (array[i] > low_key) {
                return false;
            }
        }
        return false;
    } else if (container->type == CONTAINER_BITMAP) {
        uint64_t *bitmap = (uint64_t *)container->data;
        uint64_t mask = (1ULL << (low_key % 64));
        return (bitmap[low_key / 64] & mask) != 0;
    }
    return false;
}

uint64_t deletion_vector_get_simd_mask(const deletion_vector_t *dv, uint32_t start_row) {
    uint16_t high_key = start_row >> 16;
    uint16_t low_key = start_row & 0xFFFF;

    // find the container
    roaring_container_t *container = NULL;
    size_t insert_idx = 0;
    for (; insert_idx < dv->num_containers; insert_idx++) {
        if (dv->keys[insert_idx] == high_key) {
            container = dv->containers[insert_idx];
            break;
        } else if (dv->keys[insert_idx] > high_key)
            break;
    }
    if (!container)
        return 0;
    if (container->type == CONTAINER_ARRAY) {
        uint64_t out_mask = 0;
        uint16_t *array = (uint16_t *)container->data;
        for (size_t i = 0; i < container->cardinality; i++) {
            if (array[i] >= low_key && array[i] < low_key + 64) {
                out_mask |= (1ULL << (array[i] - low_key));
            } else if (array[i] >= low_key + 64) {
                break;
            }
        }
        return out_mask;
    } else if (container->type == CONTAINER_BITMAP) {
        uint64_t *bitmap = (uint64_t *)container->data;
        return bitmap[low_key / 64];
    }
    return 0;
}
```

---

> **Day 13 Branch**: View the exact code for this day at [dmanchon/simpledb (`day13`)](https://github.com/dmanchon/simpledb/tree/day13) ([diff from `day12`](https://github.com/dmanchon/simpledb/compare/day12...day13)).
