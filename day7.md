# Day 7: Bitmaps & Dynamic Schema Registry

In analytical databases, handling missing or `NULL` data efficiently is required. Storing a dedicated flag per value (like a boolean array) wastes memory bandwidth. Similarly, ingesting data often involves semi-structured, schema-less formats like JSON, requiring the database to discover and manage schemas dynamically.

This section covers implementing **Bitmaps** to track validity at a cost of 1 bit per row, and building a **Dynamic Schema Registry** that parses JSON events directly into columnar array buffers. These in-memory buffers will eventually be flushed to disk via a column writer, which is covered in Day 9.

## 1. The Cost of Nullability

In analytical workloads, data is often sparse or missing. A mechanism is needed to mark a value as `NULL` without corrupting aggregations (like SUM or AVG). 

### Alternatives & Drawbacks:
1. **Sentinel Values (e.g., using `INT_MIN` or `-1` for NULL):** 
   This corrupts the domain (it is no longer possible to store `-1` as valid data). More importantly, checking `if (val == INT_MIN)` breaks vectorization and SIMD pipelines due to branching.
2. **Boolean Arrays (`bool is_null[N]`):**
   In C, a `bool` takes up 1 byte (8 bits). For 1 million rows, this wastes 1 Megabyte of memory bandwidth just to read the null states. This degrades cache locality.

### Bit-Packed Bitmaps
This approach uses a single bit per row. An array of 1,000,000 rows takes exactly 125 KB instead of 1 MB. Furthermore, using bits enables bitwise `AND`/`OR` operations across multiple columns simultaneously without loops, reducing overhead for null-aware filtering.

## 2. Dynamic Schema Registry

When ingesting semi-structured formats like JSON from telemetry or logs, schemas are frequently mutated by upstream applications. Traditional schema management requires predefined structures.

### Alternatives & Drawbacks:
1. **Schema-on-Read (Store raw JSON strings):**
   The database stores the raw string, parsing the JSON at query time (e.g., Postgres `JSONB`). This increases parsing overhead at query time and limits columnar SIMD execution.
2. **Strict Static Schema (`CREATE TABLE`):**
   If an incoming JSON event has an unmapped field, it is either dropped or it halts the ingestion pipeline.

### Dynamic Schema-on-Write
During ingestion, the registry inspects incoming keys. Known keys are routed to the corresponding columnar buffer. Unrecognized keys are dynamically registered, triggering the allocation of a new column buffer. This provides the flexibility of schema-less JSON alongside the performance profile of strictly-typed arrays.

### Query Execution Context
The dynamic discovery only happens during **INSERTs**. 
During ingestion, the system extracts the values and packs them into dense, 64-byte aligned SIMD arrays (built in previous sections). 

During a **SELECT** query (e.g. `SELECT sum(new_field) FROM events WHERE status = 'active'`), the query planner requests the index from the registry. The registry returns integer indexes (e.g., column `2` and column `1`). The execution engine then retrieves those arrays and iterates through them. The query engine requires no context regarding the original JSON structure. Once these in-memory arrays reach a certain threshold, they are serialized to disk. Day 9 introduces the `column_writer_t` to handle flushing these contiguous arrays into binary files.

### `int32_t` Data Representation
The engine relies heavily on `int32_t` arrays. A 32-bit signed integer (represented by `COL_TYPE_I32`) efficiently represents:
1. **Whole Numbers:** IDs, quantities, HTTP status codes.
2. **Dates:** Days since Unix Epoch.
3. **Timestamps:** Seconds since Unix Epoch (overflows in 2038; 64-bit integers preferred in production).
4. **Fixed-Point Currency:** Storing `$12.34` as `1234` (multiplying by 100) avoids floating-point precision loss.
5. **Strings (Dictionaries):** String values are stored once in a Hash Map (Day 6). The column array stores the integer dictionary ID.

### High-Cardinality Strings
Dictionary encoding degrades rapidly for high-cardinality data (e.g., unique UUIDs, raw log messages, free-text). Millions of unique strings cause hash map memory bloat and CPU cache thrashing. 
For these workloads, engines use a flat String layout (similar to Apache Arrow):
1. A contiguous byte array (the string heap/arena) holding all raw characters sequentially.
2. An `int32_t` array storing the starting byte offset of each string.
This avoids hash map overhead and maintains spatial locality for sequential scans.

#### Industry Comparisons
- **ClickHouse:** Uses `LowCardinality(String)` for dictionary encoding. For high-cardinality, it defaults to raw `String`, utilizing the contiguous byte arena and offset array architecture.
- **DuckDB:** Uses a custom 16-byte `string_t` format. Strings under 12 bytes are inlined directly into the struct. Larger strings store a 4-byte prefix (for early-exit comparisons) and a pointer to a separate string block.
- **Apache Arrow:** Implements the standard `StringArray` layout: a flat `int32_t` offsets buffer mapping into a single flat character buffer.
- **Postgres:** Uses TOAST (The Oversized-Attribute Storage Technique). Strings are stored inline within the row tuple up to 2KB. Larger strings are compressed and stored out-of-line in hidden tables. This requires pointer chasing and impacts analytical scan locality.

### Floats and Nullability
The registry extends beyond integers:
1. **Floats (`f32`):** `COL_TYPE_F32` routes decimal values (e.g., `3.14`) to a `buffer_f32_t` array, preserving IEEE 754 floating-point precision.
2. **Nullability:** All column types (`COL_TYPE_I32`, `COL_TYPE_F32`, `COL_TYPE_STRING_DICT`) are nullable. The null state is stored out-of-band in the `bitmap_t`. This keeps the numeric arrays perfectly dense for SIMD vectorization while the bitmap tracks missing data.

## 3. Header File Signatures

The following header contracts define the bitmap and schema registry structures:

### `bitmap.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t* bits;
    size_t   size;      // Number of tracked bits (rows)
    size_t   capacity;  // Allocated capacity in bytes
} bitmap_t;

bitmap_t* bitmap_create(size_t initial_capacity_bytes);
void           bitmap_free(bitmap_t* bitmap);

// Set the bit at `index` to 1 (meaning the value IS NULL)
void bitmap_set_null(bitmap_t* bitmap, size_t index);

// Set the bit at `index` to 0 (meaning the value is NOT NULL)
void bitmap_clear_null(bitmap_t* bitmap, size_t index);

// Check if the value at `index` is NULL
bool bitmap_is_null(const bitmap_t* bitmap, size_t index);
```

### `schema_registry.h`
```c
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAX_COLUMN_NAME_LEN 64

typedef enum {
    COL_TYPE_I32,
    COL_TYPE_F32,
    COL_TYPE_STRING_DICT
} column_type_t;

typedef struct {
    char          name[MAX_COLUMN_NAME_LEN];
    column_type_t type;
} schema_column_t;

typedef struct {
    schema_column_t* columns;
    size_t           count;
    size_t           capacity;
} schema_registry_t;

schema_registry_t* schema_registry_create(void);
void               schema_registry_free(schema_registry_t* registry);

// Registers a new column and returns its index, or returns existing index if already present
int32_t schema_registry_add_column(schema_registry_t* registry, const char* name, column_type_t type);

// Returns the index of the column, or -1 if not found
int32_t schema_registry_get_column_index(const schema_registry_t* registry, const char* name);
```

## 4. Implementation Snippets

### Memory Allocation in C
When building database structures, the allocation of the metadata container (the struct) must be separated from the allocation of the backing data buffer. 

For example, when creating the bitmap, allocating the required bit capacity and assigning it directly to the `bitmap_t*` pointer means any struct field assignments (like `bitmap->size = 0`) will overwrite the first few bytes of the raw bit array, and the internal pointer (`bitmap->bits`) will remain uninitialized. Two distinct allocations are necessary: one for the struct itself, and one for the internal buffer it manages.

### Hardware-Level Bit Manipulation
Bitwise math is used to set, clear, or check a bit. Using bitwise operators (`|`, `&`, `~`, `<<`) avoids branching instructions. This prevents branch mispredictions in the CPU pipeline, ensuring SIMD vectorization pipelines remain uninterrupted.

#### The Binary Mask
Since 8 bits are packed into every byte, the target byte is found by dividing the index by 8 (`index / 8`). 

The specific bit within that byte is determined using modulo (`index % 8`). A mask targets this bit. 

By taking the number `1` (which is `00000001` in binary) and left-shifting it (`<<`) by the remainder, the `1` is aligned to the target position.
- If `index % 8 == 3`, `1 << 3` creates the mask `00001000`.

Bitwise logic details:
- **`~` vs `!`**: The `~` (bitwise NOT) operator is used for clearing, rather than `!` (logical NOT). If the mask is `00001000`, applying `~` inverts it to `11110111`. Using bitwise `&` against the byte forces only the 3rd bit to `0` while leaving the other 7 bits untouched. Using `!` would evaluate to `0`, wiping the entire byte.
- **`&` vs `&&`**: The `&` (bitwise AND) operator is used for checking. Logical AND (`&&`) checks if the byte is non-zero, which returns true if any bit is set.
- **`!= 0` vs `== 1`**: Masking the 3rd bit (`00001000`) results in the integer value `8`, not `1`. Comparing the result to `> 0` or `!= 0` safely confirms the bit was set regardless of its decimal value.

### Bit-Packing Operations
```c
bitmap_t *bitmap_create(size_t initial_capacity_bytes) {
    // 1. Allocate the metadata struct
    bitmap_t *bitmap = malloc(sizeof(bitmap_t));
    if(!bitmap) {
        fprintf(stderr, "[FATAL ERROR] bitmap_create failed to allocate bitmap_t struct\n");
        exit(EXIT_FAILURE);
    }
    
    bitmap->size = 0;
    bitmap->capacity = initial_capacity_bytes;

    // 2. Allocate the raw bit buffer and zero it out (default to NOT NULL)
    bitmap->bits = (uint8_t *)calloc(initial_capacity_bytes, sizeof(uint8_t));
    if(!bitmap->bits) {
        fprintf(stderr, "[FATAL ERROR] bitmap_create failed to allocate %zu bytes for bit array\n", initial_capacity_bytes);
        exit(EXIT_FAILURE);
    }
    
    return bitmap;
}

void bitmap_free(bitmap_t *bitmap) {
    if(!bitmap) return;

    free(bitmap->bits);
    free(bitmap);
}

void bitmap_set_null(bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        fprintf(stderr, "[FATAL ERROR] bitmap index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    bitmap->bits[slot] |= 1<<(index%8);
}

void bitmap_clear_null(bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        fprintf(stderr, "[FATAL ERROR] bitmap index out of bounds\n");
        exit(EXIT_FAILURE);
    }
    bitmap->bits[slot] &= ~(1<<(index%8));
}

bool bitmap_is_null(const bitmap_t *bitmap, size_t index) {
    size_t slot = index / 8;
    if(slot >= bitmap->capacity) {
        return false;
    }
    return (bitmap->bits[slot] & (1<<(index%8))) > 0;
}
```

### Schema Registry Implementation
```c
#include "simpledb/schema_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_SCHEMA_CAPACITY 100

schema_registry_t *schema_registry_create(void) {
    schema_registry_t *r = malloc(sizeof(schema_registry_t));
    if(!r) {
        fprintf(stderr, "[FATAL ERROR] OOM cannot create schema_registry\n");
        exit(EXIT_FAILURE);
    }

    r->columns = calloc(INITIAL_SCHEMA_CAPACITY, sizeof(schema_column_t));
    if(!r->columns) {
        fprintf(stderr, "[FATAL ERROR] OOM cannot allocate schema_columns array\n");
        exit(EXIT_FAILURE);
    }
    
    r->count = 0;
    r->capacity = INITIAL_SCHEMA_CAPACITY;

    return r;
}

void schema_registry_free(schema_registry_t *registry) {
    if(!registry) return;
    free(registry->columns);
    free(registry);
}

int32_t schema_registry_get_column_index(const schema_registry_t *registry, const char *name) {
    if(!registry || !name) return -1;

    for(size_t i = 0; i < registry->count; i++) {
        if(strcmp(registry->columns[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t schema_registry_add_column(schema_registry_t *registry, const char *name, column_type_t type) {
    if(!registry || !name) return -1;

    int32_t index = schema_registry_get_column_index(registry, name);
    if(index != -1) {
        if(registry->columns[index].type != type) {
            fprintf(stderr, "[FATAL ERROR] Schema mismatch for column '%s'\n", name);
            exit(EXIT_FAILURE);
        }
        return index;
    }

    if(registry->count >= registry->capacity) {
        registry->capacity *= 2;
        registry->columns = realloc(registry->columns, registry->capacity * sizeof(schema_column_t));
        if(!registry->columns) {
            fprintf(stderr, "[FATAL ERROR] OOM cannot reallocate schema_columns array\n");
            exit(EXIT_FAILURE);
        }
    }

    size_t new_index = registry->count;
    strncpy(registry->columns[new_index].name, name, MAX_COLUMN_NAME_LEN - 1);
    registry->columns[new_index].name[MAX_COLUMN_NAME_LEN - 1] = '\0';
    registry->columns[new_index].type = type;
    
    registry->count++;
    return (int32_t)new_index;
}
```

## 5. Final Tests

Here is the test harness to verify the implementations of the bitmap and schema registry.

```c
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "bitmap.h"
#include "schema_registry.h"

void test_bitmap() {
    printf("Testing Bitmap...\n");
    bitmap_t* bmp = bitmap_create(8); // 8 bytes = 64 bits
    
    // Initially everything should be NOT NULL (0) if zero-initialized
    assert(bitmap_is_null(bmp, 5) == false);
    
    bitmap_set_null(bmp, 5);
    assert(bitmap_is_null(bmp, 5) == true);
    assert(bitmap_is_null(bmp, 4) == false);
    
    bitmap_clear_null(bmp, 5);
    assert(bitmap_is_null(bmp, 5) == false);
    
    bitmap_free(bmp);
    printf("Bitmap tests passed!\n");
}

void test_schema_registry() {
    printf("Testing Schema Registry...\n");
    schema_registry_t* reg = schema_registry_create();
    
    int32_t idx1 = schema_registry_add_column(reg, "user_id", COL_TYPE_I32);
    int32_t idx2 = schema_registry_add_column(reg, "status", COL_TYPE_STRING_DICT);
    
    assert(idx1 == 0);
    assert(idx2 == 1);
    assert(reg->count == 2);
    
    int32_t lookup_idx = schema_registry_get_column_index(reg, "status");
    assert(lookup_idx == 1);
    
    int32_t missing_idx = schema_registry_get_column_index(reg, "salary");
    assert(missing_idx == -1);
    
    schema_registry_free(reg);
    printf("Schema Registry tests passed!\n");
}

int main() {
    test_bitmap();
    test_schema_registry();
    printf("All Day 7 tests passed!\n");
    return 0;
}
```
