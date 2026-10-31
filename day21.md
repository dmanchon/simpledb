# Day 21: The Interactive REPL & Write Path (Dynamic Schema Evolution & Streaming NDJSON Ingestion)

> **Day 21 Branch**: View the exact code for this day at [dmanchon/simpledb (`day21`)](https://github.com/dmanchon/simpledb/tree/day21) ([diff from `day20`](https://github.com/dmanchon/simpledb/compare/day20...day21)).

> [!TIP]
> **Long-Read Engineering Log (~890 lines) — TL;DR & Fast Navigation**
> This post is an unabridged Socratic pair-programming log capturing the architecture, C11 pointer mechanics, and edge cases encountered while building the interactive REPL and write path.
> - **Dynamic Schema Evolution (`table_ensure_column`):** Registers newly discovered columns on the fly and immediately backfills existing in-memory rows (`table->row_count`) with default `0` or `""` so columnar arrays never become jagged.
> - **Single-Row Point Inserts (`repl_exec_insert`):** Binds out-of-order SQL `INSERT INTO ... VALUES (...)` columns to physical schema indices in a single pass.
> - **Streaming NDJSON Ingestion (`repl_exec_bulk_insert`):** Uses `yyjson` in two phases—an unconditional $N$-row union schema pre-scan (`rewind(fp)`) followed by bounded `1,024`-row chunked batch ingestion in $O(1)$ RAM.
> - **Want to go straight to the code?** [Jump to the Unit Tests](#7-unit-testing-the-write-path-teststest_dbc) or [Jump to the Final C11 Implementation (`src/repl.c`)](#8-the-final-listing-srcreplc--write-path--repl-loop).

Welcome to Day 21! Over the previous 20 days, we have built all the core components of a vectorized, columnar analytical database engine in C11:
- Zero-copy memory-mapped columnar storage (Days 1–3)
- Vectorized SIMD filtering and bitmap selection vectors (Days 5–6)
- Dynamic schema registry and dictionary encoding (Days 7–8)
- Inverted indexes and BM25 text search (Days 9–11)
- Columnar table abstraction, active RAM buffers, and immutable disk parts (Days 14–16)
- Table scanning, projections, and predicate pushdown (Day 17)
- Vectorized query execution pipeline and Grace Hash Aggregation (Day 18)
- K-way Min-Heap part merging and background multi-threaded compaction (Day 19)
- Minimal SQL & K/q Query Parser and AST generator (Day 20)

Today, we bring all of these pieces together into an interactive **REPL (Read-Eval-Print Loop) CLI** and **Query Engine**. By the end of this day, you will be able to launch `simpledb`, stream real-world Newline-Delimited JSON (NDJSON) datasets into columnar storage with automatic schema inference, execute vectorized analytical SQL queries, and trigger background compaction—all from an interactive terminal shell!

---

## 1. Architecture: The REPL as the System Coordinator

At first glance, it might look like the REPL is just a simple while-loop printing text. In a modern analytical database, the REPL acts as the **top-level Coordinator and Query Planner** sitting above three subsystems:

```
                       +------------------------+
   User SQL String --->|  sql_parser_parse()    | (Day 20: Lexer + AST Generator)
                       +-----------+------------+
                                   |
                              ast_query_t
                                   |
              +--------------------+--------------------+
              |                                         |
        [STMT_INSERT]                             [STMT_SELECT]
              v                                         v
   +-----------------------+               +---------------------------+
   |   repl_exec_insert()  |               |    repl_exec_select()     |
   |  (Write Coordinator)  |               |   (Mini Query Planner)    |
   +-----------+-----------+               +-------------+-------------+
               |                                         |
   table_insert_batch() /                  Builds physical_operator_t tree:
   table_flush() (Day 14)                  OP_SCAN -> OP_FILTER -> OP_AGGREGATE
               |                                         |
               v                                         v
   +-----------------------------------+   +---------------------------+
   |         table_t Catalog           |<--|   exec_pipeline_run()     |
   | Active RAM Buffers + Disk Parts   |   | (Day 18 Vectorized Engine)|
   +-----------------+-----------------+   +---------------------------+
                     ^
                     |  Wakes up every 5s (pthread_rwlock + atomic refcount)
                     v
   +-----------------------------------+
   | table_background_compactor_thread |
   |   (Day 19 Background pthread)     |
   +-----------------------------------+
```

1. **The Write Path (`repl_exec_insert`):** Translates SQL `INSERT` statements or streaming NDJSON records into columnar vectors and appends them to active RAM buffers using `table_insert_batch()`. When RAM buffers reach capacity (or when the user issues `.flush`), it flushes an immutable disk part directory (`all_1_1_0`, `all_2_2_0`, ...).
2. **The Read Path (`repl_exec_select` + `exec_pipeline_run`):** Translates `ast_query_t` into a physical operator tree (`OP_SCAN` $\to$ `OP_FILTER` $\to$ `OP_AGGREGATE`). Day 18's vectorized engine executes the pipeline in batches of 1,024 rows and prints the result as an ASCII table.
3. **The Background Compactor:** Launches in a detached POSIX thread (`table_start_background_compactor`). While the main thread sits blocked waiting for user input on `stdin`, the compactor silently wakes up, discovers mergeable parts, merges them using our Day 19 K-way Min-Heap, and garbage-collects obsolete files.

---

## 2. The Header: `include/simpledb/repl.h`

Because the REPL does not own heap state across runs, we do not need an unnecessary `repl_t` wrapper struct or `repl_create()` / `repl_destroy()` boilerplate. In addition to `repl_run()`, we expose `repl_exec_insert()` and `repl_exec_select()` so individual AST queries can be directly verified in unit tests without having to mock interactive `stdin`:

```c
#pragma once

#include "simpledb/table.h"
#include "simpledb/sql_parser.h"

void repl_run(table_t *db);
void repl_exec_insert(table_t *db, ast_query_t *ast);
void repl_exec_select(table_t *db, ast_query_t *ast);
```

---

## 3. The Interactive Read-Eval-Print Loop

### How POSIX `getline()` Uses `cap` and Reuses Memory

Terminal input is handled using POSIX `getline(&line, &cap, stdin)` inside a `while (true)` loop:

- **Allocated Buffer Capacity (`cap`):** `cap` tracks the physical capacity in bytes of the heap buffer currently pointed to by `line`.
- **First Call (`line == NULL, cap == 0`):** `getline` automatically allocates an initial buffer (typically 128 bytes) and populates `cap` and `line`.
- **Automatic Dynamic Resizing:** If a user pastes a 4,000-character SQL query, `getline` internally calls `realloc()` to expand the buffer and updates `cap` accordingly.
- **Zero-Allocation Reuse:** On subsequent iterations of the prompt loop, passing the exact same `&line` and `&cap` back into `getline()` reuses the existing heap buffer without performing a single new `malloc()`! We only call `free(line)` once when exiting the REPL.

### C Quirk: `size_t` vs. `ssize_t`

| Property | `size_t` | `ssize_t` |
| :--- | :--- | :--- |
| **Standard** | ISO C (`<stddef.h>`) | POSIX (`<sys/types.h>`) |
| **Signedness** | Unsigned | Signed |
| **Range (64-bit)** | `0 .. 2^64 - 1` | `-1 .. 2^63 - 1` |
| **printf Format** | `%zu` | `%zd` |
| **Primary Use** | Object sizes, counts, array bounds | Return values that may return `-1` on error/EOF |

- **Why `getline` returns `ssize_t`:** It must return either the number of bytes read (`>= 0`) or `-1` to signal `EOF` (such as `Ctrl+D`). If assigned to an unsigned `size_t`, `-1` implicitly converts to `SIZE_MAX` (`18446744073709551615`), making `< 0` checks dead code.
- **Robust Line Trimming (CRLF):** Rather than checking only `\n`, trimming backwards removes both carriage returns (`\r`) and newlines (`\n`) for clean handling of terminal inputs and redirected files:
  ```c
  while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
      line[--nread] = '\0';
  }
  if (nread == 0) continue;
  ```

### Meta-Commands (Dot-Commands)

The REPL supports special administrative commands:
- `.exit` / `.quit`: Terminates the interactive session.
- `.flush`: Materializes all active columnar RAM buffers into an immutable disk part.
- `.compact`: Triggers an on-demand compaction run (equivalent to ClickHouse's `OPTIMIZE TABLE ... FINAL`).

```c
// Core REPL loop structure
void repl_run(table_t *db) {
    if (!db) return;

    char *line = NULL;
    size_t cap = 0;
    while (true) {
        printf("simpledb> ");
        fflush(stdout);
        ssize_t nread = getline(&line, &cap, stdin);
        if (nread == -1) break; // EOF (Ctrl+D)

        while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r')) {
            line[--nread] = '\0';
        }
        if (nread == 0) continue;

        if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) break;
        if (strcmp(line, ".flush") == 0) {
            table_flush(db, ".");
            printf("Flushed active memory buffers to disk part.\n");
            continue;
        }
        if (strcmp(line, ".compact") == 0) {
            merge_config_t cfg = {.max_parts_to_merge = 10, .min_rows_to_merge = 1, .force_merge_all = true};
            part_merger_t *merger = part_merger_create(db, cfg);
            if (merger->num_parts > 0) part_merger_execute(merger);
            part_merger_destroy(merger);
            printf("Compacted disk parts (now %zu active part(s)).\n", db->num_parts);
            continue;
        }

        sql_parser_t parser = sql_parser_create(line);
        ast_query_t ast = {0};
        if (!sql_parser_parse(&parser, &ast)) {
            printf("Syntax error.\n");
            continue;
        }

        if (ast.stmt_type == STMT_INSERT) {
            repl_exec_insert(db, &ast);
        } else if (ast.stmt_type == STMT_SELECT) {
            repl_exec_select(db, &ast);
        }
    }
    free(line);
}
```

---

## 4. The Schema Catch-22: `CREATE TABLE` vs. Dynamic Schema-on-Write

When a user initializes an empty table (`table_create("events")`), `table->schema` is `NULL` and `table->column_buffers` is `NULL`.

- Relational engines (Postgres, MySQL) resolve this by enforcing an explicit `CREATE TABLE (id INT, name VARCHAR)` DDL statement before permitting inserts.
- Modern analytical systems (DuckDB `read_json_auto`, ClickHouse `JSONEachRow`, Snowflake) implement **Dynamic Schema-on-Write**: column types are automatically inferred upon the arrival of the first data payload.

### The Two Critical Requirements of `table_ensure_column`

A naive implementation might simply iterate over `table->schema->count` and call `schema_registry_add_column()`. However, in a columnar database engine, this runs into two architectural requirements:

1. **Lazy Registry Allocation (Avoiding the `NULL` Dereference):**
   When `table_create()` is called, `table->schema` is initialized to `NULL`. Attempting `table->schema->count` on the very first insert would cause an immediate `SIGSEGV`. Therefore, we must lazily instantiate the registry:
   ```c
   if (!table->schema) {
       table->schema = schema_registry_create();
   }
   ```
   *(Note: Day 7 already provides [`schema_registry_get_column_index()`](file:///Users/dmanchon/repos/cdb/src/schema_registry.c#L48-L55), which performs this lookup cleanly without writing a manual for-loop).*

2. **Mirroring Schema into Physical Column Buffers (`table->column_buffers`):**
   In Day 14, [`table_insert_batch()`](file:///Users/dmanchon/repos/cdb/src/table.c#L84-L89) performs a strict validation check:
   ```c
   if (!table || !table->schema || !table->column_buffers) {
       return -1;
   }
   if (num_columns != table->schema->count) {
       return -1;
   }
   ```
   A logical schema is just metadata. To actually store columnar data, every registered column index `i` **must** have a corresponding in-memory columnar vector at `table->column_buffers[i]` (`buffer_i32_t` for `COL_TYPE_I32` or `raw_string_column_t` for strings).

When registering a new column, we must simultaneously reallocate `table->column_buffers` and construct the active in-memory buffer. Returning the resolved column index (`int32_t idx`) provides the exact slot position:

```c
static int32_t table_ensure_column(table_t *table, const char *name, column_type_t type) {
    if (!table->schema) {
        table->schema = schema_registry_create();
    }
    int32_t idx = schema_registry_get_column_index(table->schema, name);
    if (idx == -1) {
        idx = schema_registry_add_column(table->schema, name, type);
        // Expand table->column_buffers to mirror the new schema column count
        table->column_buffers = realloc(table->column_buffers, sizeof(void *) * table->schema->count);
        if (type == COL_TYPE_I32) {
            table->column_buffers[idx] = buffer_i32_create(128);
        } else {
            table->column_buffers[idx] = raw_string_column_create(128);
        }
    }
    return idx;
}
```

### Direct Semantic Binding vs. Nested Loop Alignment

When a user writes:
```sql
INSERT INTO events (status, user_id) VALUES ('ACTIVE', 42);
```

An intuitive first attempt might be an $O(\text{schema\_cols} \times \text{query\_cols})$ nested loop: for every column in the table, scan through the AST with `strcmp` to find the matching value. 

That approach feels clunky because it does semantic binding backwards! In compiler and database architecture, the SQL AST yields a collection of **`(column_name, value)` tuples**. 

Instead of searching backwards through strings, we perform **Semantic Binding**:
1. Pre-calculate the maximum potential schema size: `(db->schema ? db->schema->count : 0) + ast->num_insert_cols`.
2. Allocate temporary slot buffers sized to this upper bound.
3. In a **single pass**, iterate through each `(col, val)` tuple, call `table_ensure_column()` to resolve or register its index `idx`, and immediately store the value into slot `idx` in $O(1)$!

```c
static void repl_exec_insert(table_t *db, ast_query_t *ast) {
    if (!db) return;

    if (ast->is_bulk_insert) {
        repl_exec_bulk_insert(db, ast->from_file_path);
        return;
    }

    size_t max_cols = (db->schema ? db->schema->count : 0) + ast->num_insert_cols;
    if (max_cols == 0) return;

    // Temporary arrays sized safely to the maximum potential column count
    int32_t *i32_vals = calloc(max_cols, sizeof(int32_t));
    const char **str_vals = calloc(max_cols, sizeof(char *));
    const void **col_ptrs = calloc(max_cols, sizeof(void *));

    for (size_t c = 0; c < max_cols; c++) {
        str_vals[c] = "";
    }

    // Fused Loop: Ensure column AND bind value directly into slot idx in one pass
    for (size_t k = 0; k < ast->num_insert_cols; k++) {
        column_type_t type = ast->insert_vals[k].is_string ? COL_TYPE_STRING : COL_TYPE_I32;
        int32_t idx = table_ensure_column(db, ast->insert_cols[k], type);
        if (idx < 0) continue;

        if (type == COL_TYPE_I32) {
            i32_vals[idx] = ast->insert_vals[k].int_val;
        } else {
            str_vals[idx] = ast->insert_vals[k].string_val;
        }
    }

    // Wire columnar double-pointers for table_insert_batch
    size_t num_cols = db->schema->count;
    for (size_t c = 0; c < num_cols; c++) {
        if (db->schema->columns[c].type == COL_TYPE_I32) {
            col_ptrs[c] = &i32_vals[c];
        } else {
            col_ptrs[c] = &str_vals[c];
        }
    }

    int rc = table_insert_batch(db, (const void ***)col_ptrs, num_cols, 1);
    if (rc == 0) {
        printf("INSERT 1 (table now has %zu rows in RAM).\n", db->row_count);
    } else {
        printf("Error: insert failed.\n");
    }

    free(i32_vals);
    free(str_vals);
    free(col_ptrs);
}
```

---

## 5. High-Throughput Ingestion: Why NDJSON and `yyjson`?

### Why Not Hand-Roll a JSON Parser?
RFC 8259 JSON compliance requires handling escaped control sequences (`\n`, `\t`, `\"`), Unicode surrogate pairs (`\uXXXX`), scientific floating-point representations (`-1.23e-4`), and arbitrarily deep nesting. Writing an ad-hoc JSON parser in pure C easily leads to buffer overflows and memory corruption.

### Monolithic JSON Arrays vs. Newline-Delimited JSON (NDJSON)
In production analytics, large datasets are never formatted as a single massive array `[ {...}, {...} ]`:
1. **$O(N)$ Memory Consumption:** A 50GB JSON array requires building a 50GB+ DOM tree in memory before the first row can be written to columnar storage.
2. **$O(1)$ Bounded Memory with NDJSON:** **Newline-Delimited JSON (NDJSON / JSONEachRow)** stores one JSON object per line. Lines are streamed with `getline()`, decoded into columnar batches of 1,024 rows, and appended directly to RAM buffers. Memory usage remains bounded to a few kilobytes regardless of whether the file contains 1,000 or 1,000,000,000 rows!

### Vendoring `yyjson`
We vendor **`yyjson`**, the fastest pure C99 JSON parser available:
- `include/yyjson.h`
- `src/yyjson.c`

### Automatic Schema Inference: Single-Row Peek vs. N-Row Sampling

A crucial question arises during file ingestion on a schema-less table: **how many rows should we inspect to infer the schema?**

1. **Single-Row Peek (The 1-Row Fast Path):**
   - We read line 1, parse with `yyjson_read()`, register every discovered key/type using `table_ensure_column()`, and `rewind(fp)`.
   - *Limitation (Sparse / Ragged JSON):* If row 1 only contains `{"user_id": 1, "status": "OK"}`, but row 2 adds optional fields `{"user_id": 2, "status": "OK", "latency": 45, "country": "US"}`, a 1-row peek misses `latency` and `country`.
2. **N-Row Sampling (DuckDB's `read_json_auto` Model):**
   - Modern analytical engines sample the first $N$ rows (e.g., 100 or 20,480 rows in DuckDB) in a quick pre-scan loop to compute the **union schema** of all observed keys and types, and then call `rewind(fp)`.
   - If an optional field only appears in 1% of rows, sampling 100 rows gives a much higher probability of capturing the full schema before allocating chunk buffers.

#### The Four Primitives of Schema Inference

When a table is freshly created via `table_create()`, `db->schema` is `NULL`. Attempting to read `db->schema->count` immediately will dereference a `NULL` pointer and segfault. To populate `db->schema` from the NDJSON stream before allocating column chunk buffers, we combine four primitives:

1. **Line Streaming (`getline`):**
   POSIX `getline(&line, &cap, fp)` reads a full line up to `\n` and dynamically `realloc`s `line` as needed, returning the number of bytes read (or `-1` on `EOF`).
2. **DOM Parsing (`yyjson_read` & `yyjson_doc_get_root`):**
   After stripping trailing `\r`/`\n`, `yyjson_read(line, len, 0)` parses the JSON object in-place and `yyjson_doc_get_root(doc)` retrieves the root JSON value.
3. **Key-Value Discovery (`yyjson_obj_foreach`):**
   Because we do not know the column names ahead of time, we iterate over every key-value pair in the JSON object using `yyjson`'s iterator macro and register each key with `table_ensure_column()`:
   ```c
   size_t idx, max;
   yyjson_val *k, *v;
   yyjson_obj_foreach(root, idx, max, k, v) {
       const char *col_name = yyjson_get_str(k);
       column_type_t col_type = (v && yyjson_is_int(v)) ? COL_TYPE_I32 : COL_TYPE_STRING;
       table_ensure_column(db, col_name, col_type);
   }
   ```
   Because `table_ensure_column()` is idempotent (returning the existing index if a column is already registered, or adding it if `idx == -1`), running this across `SCHEMA_SAMPLE_ROWS` (e.g., 100 lines) naturally computes the **union schema** of all sampled rows.

   > [!NOTE]
   > **Why Don't We `free(k)` or `free(v)`?**
   > Notice that `yyjson_val *k, *v` (and `root`) are never individually freed! Much like the **Arena Allocator** we built on Day 1, `yyjson_read()` allocates a single contiguous memory buffer inside `yyjson_doc` rather than calling `malloc()` for every key and value node. `k`, `v`, and `yyjson_get_str(k)` are merely interior pointers into `doc`'s arena. Calling `yyjson_doc_free(doc)` at the end of the line frees the entire DOM tree and all its strings in one $O(1)$ deallocation. Because those string pointers expire as soon as `yyjson_doc_free(doc)` runs, any string that outlives the current line (such as column names in `schema_registry_add_column` or string values in the chunk buffer) must be copied via `strncpy` or `strdup` before `yyjson_doc_free(doc)`.

4. **Stream Reset (`rewind`):**
   Because the sampling loop advances the `FILE *fp` cursor forward, calling `rewind(fp)` resets the file offset back to byte `0` so the main ingestion loop reads from the very first record.

### The Two-Phase Chunked Streaming Architecture

To achieve high throughput without memory exhaustion, NDJSON ingestion follows a two-phase streaming model:

1. **Phase 1: Union Schema Discovery (Unconditional Sampling Pre-Scan)**
   - Always pre-scan up to `SCHEMA_SAMPLE_ROWS` (e.g., 10–100 lines) using `getline()`—even if `db->schema` is already non-NULL from prior `INSERT` statements!
   - Why run the pre-scan unconditionally? If a table already has 3 columns (`user_id`, `status`, `amount`) and 2 rows in RAM, and an incoming NDJSON file introduces a 4th column (`region`), running the pre-scan calls `table_ensure_column(db, "region", COL_TYPE_STRING)`, which registers the 4th column and immediately backfills the 2 existing rows before chunk buffers are allocated.
   - Once sampling concludes (or EOF is encountered), call `rewind(fp)` to reset the file stream offset back to 0.

2. **Phase 2: Bounded Chunk Ingestion ($O(1)$ RAM Footprint)**
   - Allocate an array of column chunk buffers `void **col_data = calloc(num_cols, sizeof(void *));` for each active column in `db->schema`:
     - `int32_t *col_data[c]` of capacity `BULK_CHUNK_SIZE` (e.g. 1024 elements).
     - `char **col_data[c]` of capacity `BULK_CHUNK_SIZE` for string pointers.
     - *(Note: Unlike single-row `repl_exec_insert`, where `i32_vals` and `str_vals` are flat 1D arrays that require a separate `col_ptrs[c] = &i32_vals[c]` indirection array, in bulk ingestion `col_data` **is already** an array of column buffer pointers! We can pass `(const void ***)col_data` directly to `table_insert_batch` without allocating a redundant `col_ptrs` array.)*
   - Stream line-by-line using `getline()` and `yyjson_read()`:
     - Store the parsed values at `col_data[c][chunk_rows]`. If a field is missing in that specific row (sparse JSON), default to `0` or `strdup("")`.
     - When `chunk_rows == BULK_CHUNK_SIZE` (full chunk inside the loop) or when `EOF` is reached with `chunk_rows > 0` (incomplete tail chunk after the loop):
       - Call `table_insert_batch(db, (const void ***)col_data, num_cols, chunk_rows)`.
       - Free any heap-allocated strings from the current chunk.
       - Reset `chunk_rows = 0`.
   - Free scratch chunk buffers, close `fp`, and log the updated row count (`printf("BULK INSERT (table now has %zu rows in RAM).\n", db->row_count);`) to match single-row `repl_exec_insert()`.

#### Schema-Driven vs. JSON-Driven Field Binding

When binding each parsed JSON line into `col_data[c][chunk_rows]`, how do we map between JSON keys and physical column indices?

First, note that `db->schema->columns` in `schema_registry_t` **is already** a direct array map from `column_id (c) -> schema_column_t { name, type }`. However, there are two distinct ways to traverse a row:

1. **Schema-Driven Loop (`column_id -> field_name` via `yyjson_obj_get`):**
   Iterate `for (size_t c = 0; c < num_cols; c++)` and call `yyjson_obj_get(root, db->schema->columns[c].name)`.
   - *Advantage:* Every column slot `col_data[c][chunk_rows]` is visited and populated in one loop (either with the JSON value or the default `0` / `strdup("")`), making it impossible to leave an uninitialized hole in a sparse row.
   - *Trade-off:* Because `yyjson` objects are stored as flat key-value sequences rather than hash tables, `yyjson_obj_get(root, col_name)` performs a linear scan ($O(K)$) of the JSON object for each of the $C$ columns ($O(C \times K)$).

2. **JSON-Driven Loop (`field_name -> column_id` via `yyjson_obj_foreach`):**
   Just like our single-row `repl_exec_insert()`, we can invert the loop:
   - First, initialize row `chunk_rows` across all columns `0..num_cols-1` to default values (`0` for `I32`, `NULL` for `STRING`).
   - Second, iterate the JSON object **once** using `yyjson_obj_foreach(root, idx, max, k, v)`, resolve the column index via `int32_t c = schema_registry_get_column_index(db->schema, yyjson_get_str(k));`, and write directly to `col_data[c][chunk_rows]`.
   - Third, replace any remaining `NULL` string slots at `chunk_rows` with `strdup("")`.
   - For sparse JSON rows (e.g., 2 fields present out of 30 schema columns), walking only the keys present in the JSON object avoids scanning the JSON DOM repeatedly for 28 missing columns!

   > [!IMPORTANT]
   > **Why Pre-Clearing `col_data[c][chunk_rows]` Before `yyjson_obj_foreach` Is Critical:**
   > Because `col_data[c]` is allocated with `malloc()` and reused across multiple chunks of `BULK_CHUNK_SIZE` (1,024 rows), failing to reset `col_data[c][chunk_rows]` to `0` / `NULL` before `yyjson_obj_foreach` causes two subtle bugs on sparse rows:
   > 1. On the first chunk, unpopulated slots contain uninitialized heap garbage, so `if (!arr[chunk_rows])` evaluates to `false` and passes a wild pointer to `table_insert_batch()`.
   > 2. On subsequent chunks (rows `1024+`), after `free(arr[r])` frees the previous chunk's strings, `arr[r]` still holds the old non-NULL address (a **dangling pointer**) unless explicitly set to `NULL`!

---

## 6. Schema Evolution vs. Type Conflicts

When ingesting multiple batches of data over time, schemas may drift:

### 1. Additive Conflicts (New Columns / Schema Evolution Mid-Flight)

What happens when a new column is introduced to an active table that already contains rows in RAM?
- Batch 1 has `{"user_id": 1, "status": "OK"}`. Table has `row_count = 1`.
- Batch 2 adds `amount`: `{"user_id": 2, "status": "OK", "amount": 100}`.
- If `table_ensure_column()` merely creates an empty buffer (`size = 0`), the columns become **jagged**:
  - `user_id` has 2 elements (indices 0 and 1).
  - `amount` has only 1 element (at index 0).
  - Now Row 1's `amount` is stored at index 0, corrupting physical row alignment!

**The Backfill Alignment Rule:**
Whenever a new column is added to a table with `table->row_count > 0`, the newly created buffer MUST be immediately backfilled with `table->row_count` default values (`0` for numeric columns, `""` for strings):

```c
static int32_t table_ensure_column(table_t *table, const char *name, column_type_t type) {
    if (!table->schema) {
        table->schema = schema_registry_create();
    }
    int32_t idx = schema_registry_get_column_index(table->schema, name);
    if (idx == -1) {
        idx = schema_registry_add_column(table->schema, name, type);
        table->column_buffers = realloc(table->column_buffers, sizeof(void *) * table->schema->count);
        
        size_t existing_rows = table->row_count;
        if (type == COL_TYPE_I32) {
            buffer_i32_t *buf = buffer_i32_create(existing_rows > 128 ? existing_rows * 2 : 128);
            // Pad existing in-memory rows with default 0 so columnar vectors stay aligned
            for (size_t r = 0; r < existing_rows; r++) {
                buffer_i32_push(buf, 0);
            }
            table->column_buffers[idx] = buf;
        } else {
            raw_string_column_t *str_col = raw_string_column_create(existing_rows > 128 ? existing_rows * 2 : 128);
            // Pad existing in-memory rows with default ""
            for (size_t r = 0; r < existing_rows; r++) {
                str_col->strings[r] = strdup("");
            }
            str_col->size = existing_rows;
            table->column_buffers[idx] = str_col;
        }
    }
    return idx;
}
```

*What about older rows that were already flushed to disk (`all_1_1_0`)?*
Disk parts are immutable. If `amount.bin` is missing from an older part, Day 17's Table Scanner automatically synthesizes default zero blocks on the fly during scan time.

### 2. Type Conflicts (Mutating Types):
   - Batch 1 has `{"user_id": 1}` (`COL_TYPE_I32`). Batch 2 has `{"user_id": "abc"}` (`COL_TYPE_STRING`).
   - In Day 7, `schema_registry_add_column` enforces strict fail-fast validation. Analytical databases require rigid physical column formats; altering an existing column type on disk requires explicit DDL migrations.

---

## 7. Unit Testing the Write Path (`tests/test_db.c`)

### Single-Row `VALUES` vs. Multi-Row Batch Ingestion

In Day 20's minimal parser grammar, `INSERT INTO ... VALUES (...)` is specifically designed for single-row interactive commands (`ast->insert_vals[16]`). In high-performance columnar engines:
- Single-row SQL `INSERT` is used for ad-hoc point queries and testing; it appends 1 row at a time (`num_rows = 1`).
- Multi-row bulk ingestion is handled via streaming NDJSON files (`INSERT INTO events FROM 'data.json'`) using `yyjson`, which streams in chunks of 1,024 rows to avoid SQL string concatenation bottlenecks.

> [!IMPORTANT]
> **Explicit Architectural Constraint:** `INSERT INTO <table> VALUES (...)` in `simpledb` is strictly a single-row point insert mechanism. Attempting multi-row tuples like `VALUES (1, 'a'), (2, 'b')` will only parse and insert the first tuple; any subsequent tuples are ignored. All multi-row and batch ingestion is intentionally delegated to streaming NDJSON files.

#### What Would Multi-Row `INSERT INTO ... VALUES (...), (...)` Require?

To support multi-row SQL inserts in pure C, three components must change:
1. **AST Representation:** Instead of a 1D array (`ast_value_t insert_vals[16]`), `ast_query_t` would need a 2D matrix or dynamic linked list of rows: `ast_value_t insert_rows[MAX_ROWS][MAX_COLS]`.
2. **Parser Grammar Loop:** In `sql_parser_parse_insert()`, after consuming the first `)`, the parser must enter a loop checking for `, (` to parse each subsequent row tuple until the terminating semicolon.
3. **Row-to-Column Transposition:** In `repl_exec_insert()`, the row-oriented AST tuples must be transposed into columnar contiguous memory arrays before calling `table_insert_batch(db, col_ptrs, num_cols, num_rows)`.

#### Why Analytical Databases Discourage Giant Multi-Row SQL Strings

While transactional databases (PostgreSQL, MySQL) often receive multi-row `INSERT` statements, modern analytical engines (ClickHouse, Snowflake, DuckDB) strongly discourage them:
- **String Parsing Bottleneck:** A 50MB SQL query with 100,000 tuples forces the lexer and parser to allocate and traverse millions of commas, parentheses, quotes, and string literals, exhausting CPU cycles and memory.
- **Columnar Impedance:** Databases must transpose row tuples into columnar arrays on every single query.
- **The Modern Solution:** Ingesting structured file streams (NDJSON, Parquet, Arrow) bypasses SQL parsing entirely, allowing SIMD or chunked JSON decoders to populate columnar buffers directly in bounded $O(1)$ RAM.

To insert multiple rows via SQL statements in the unit tests, execute distinct `INSERT` statements:

```c
static void test_repl_execution(void) {
    table_t *table = table_create("events_repl_test");
    assert(table != NULL);
    assert(table->schema == NULL); // Starts completely schema-less

    // 1. First INSERT: dynamically discovers (user_id: I32, status: STRING)
    {
        const char *query = "INSERT INTO events (user_id, status) VALUES (42, 'SUCCESS');";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));

        repl_exec_insert(table, &ast);

        assert(table->row_count == 1);
        assert(table->schema != NULL);
        assert(table->schema->count == 2);

        // Verify physical columnar vectors in memory
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[0] == 42);

        raw_string_column_t *str_buf = (raw_string_column_t *)table->column_buffers[1];
        assert(strcmp(str_buf->strings[0], "SUCCESS") == 0);
    }

    // 2. Second INSERT: inverted column order (status first, then user_id)
    {
        const char *query = "INSERT INTO events (status, user_id) VALUES ('FAILED', 99);";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));

        repl_exec_insert(table, &ast);

        assert(table->row_count == 2);
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[1] == 99);

        raw_string_column_t *str_buf = (raw_string_column_t *)table->column_buffers[1];
        assert(strcmp(str_buf->strings[1], "FAILED") == 0);
    }

    // 3. Third INSERT: Streaming NDJSON ingestion with union schema inference
    {
        char tmp_path[] = "/tmp/cdb_bulk_XXXXXX";
        int fd = mkstemp(tmp_path);
        assert(fd != -1);
        FILE *f = fdopen(fd, "w");
        assert(f != NULL);
        // Row 1: user_id and status
        fprintf(f, "{\"user_id\": 101, \"status\": \"PENDING\"}\n");
        // Row 2: introduces amount and region (sparse / union schema evolution)
        fprintf(f, "{\"user_id\": 102, \"status\": \"SUCCESS\", \"amount\": 250, \"region\": \"EU\"}\n");
        // Row 3: sparse row (omits status and region)
        fprintf(f, "{\"user_id\": 103, \"amount\": 99}\n");
        fclose(f); // Closes both FILE* and underlying fd

        table_t *bulk_table = table_create("events_bulk_test");
        char bulk_query[256];
        snprintf(bulk_query, sizeof(bulk_query), "INSERT INTO events_bulk_test FROM '%s';", tmp_path);

        sql_parser_t parser = sql_parser_create(bulk_query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        assert(ast.is_bulk_insert);

        repl_exec_insert(bulk_table, &ast);

        assert(bulk_table->row_count == 3);
        assert(bulk_table->schema != NULL);
        assert(bulk_table->schema->count == 4); // user_id, status, amount, region

        // Verify column 0 (user_id) values
        buffer_i32_t *user_col = (buffer_i32_t *)bulk_table->column_buffers[0];
        assert(user_col->data[0] == 101);
        assert(user_col->data[1] == 102);
        assert(user_col->data[2] == 103);

        table_destroy(bulk_table);
        remove(tmp_path);
    }

    table_destroy(table);
    printf("Day 21 REPL execution tests passed!\n");
}
```

When `test_repl_execution()` runs, the two single-row `VALUES` inserts hit the `printf("INSERT 1 (table now has %zu rows in RAM).\n", db->row_count);` statement at the bottom of `repl_exec_insert()`, while the 3rd block dispatches via `if (ast->is_bulk_insert)` to `repl_exec_bulk_insert()` and returns early:

```text
INSERT 1 (table now has 1 rows in RAM).
INSERT 1 (table now has 2 rows in RAM).
Day 21 REPL execution tests passed!
```

#### Temporary Files in C: `mkstemp()`, `tmpfile()`, and Why `tmpnam()` Was Banned

When testing SQL commands that accept a file path string (`INSERT INTO ... FROM '<path>'`), choosing the right C temporary file API reveals an important piece of UNIX systems history:

- **Why ISO C `tmpfile()` Does Not Work Here:** `FILE *tmpfile(void)` creates an anonymous temporary file and immediately `unlink`s its directory entry so it auto-deletes on close. Because it has no visible path on the filesystem, you cannot pass a filename string to the SQL parser (`FROM '...'`) for `repl_exec_bulk_insert()` to open via `fopen()`.
- **Why `tmpnam()` (Auto-Generated Path) Is Banned:** Standard C originally provided `char *tmpnam(char *s)` to automatically pick `/tmp` and generate a unique filename for you. However, because `tmpnam()` only returns a path string *without* opening the file, it suffers from a **TOCTOU (Time-Of-Check to Time-Of-Use) race condition**: between the moment `tmpnam()` checks that the path does not exist and the moment your code calls `fopen()`, another process can create a symlink at that path. Modern compilers and linkers actively emit security warnings if you call `tmpnam()`.
- **How POSIX `mkstemp()` Solves Both (`XXXXXX` Template):**
  In `char tmp_path[] = "/tmp/cdb_bulk_XXXXXX";`, the trailing `XXXXXX` is **not** a literal filename—it is a placeholder template that `mkstemp()` overwrites in-place with 6 cryptographically/pseudo-randomly generated alphanumeric characters ($62^6 \approx 56.8\text{ billion}$ combinations, e.g., `/tmp/cdb_bulk_q9Km2A`).
  Because C functions only have a single return value, `mkstemp()` returns the atomically opened `int fd` (created with `O_CREAT | O_EXCL` so no race condition is possible) and mutates your `tmp_path` array in-place to hand back the generated path with zero heap allocations!

> [!WARNING]
> **Two Common Pitfalls in Bulk Ingestion Tests:**
> 1. **Self-Contained Test Fixtures:** Running `INSERT INTO events FROM 'data.json'` requires `data.json` to exist relative to the test binary's current working directory. Using `mkstemp("/tmp/cdb_bulk_XXXXXX")` isolates the test from the working directory and prevents stray files in the repository.
> 2. **Early Return After Bulk Dispatch:** In `repl_exec_insert()`, the `if (ast->is_bulk_insert)` branch **must** end with `return;`. Otherwise, if `db->schema` is already populated from prior `INSERT` statements, execution falls through to the single-row `VALUES` path (`max_cols = db->schema->count + 0`) and inserts a phantom row of default `0`s and `""`s!

---

## 8. The Final Listing (`src/repl.c` — Write Path & REPL Loop)

Below is the complete implementation of the Day 21 REPL write path, including dynamic schema backfilling (`table_ensure_column`), two-phase streaming NDJSON ingestion (`repl_exec_bulk_insert`), single-row semantic tuple binding (`repl_exec_insert`), and the interactive command loop (`repl_run`):

```c
#include "simpledb/repl.h"
#include "simpledb/schema_registry.h"
#include "simpledb/sql_parser.h"
#include "simpledb/table.h"
#include "yyjson.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define BULK_CHUNK_SIZE 1024
#define SCHEMA_SAMPLE_ROWS 10

static int32_t table_ensure_column(table_t *table, const char *name, column_type_t type) {
    if (!table->schema) {
        table->schema = schema_registry_create();
    }
    int32_t idx = schema_registry_get_column_index(table->schema, name);
    if (idx == -1) {
        idx = schema_registry_add_column(table->schema, name, type);
        size_t existing_rows = table->row_count;
        // Expand table->column_buffers to hold the new column buffer pointer
        table->column_buffers = realloc(table->column_buffers, sizeof(void *) * table->schema->count);
        if (type == COL_TYPE_I32) {
            buffer_i32_t *buf = buffer_i32_create(128);
            for (size_t r = 0; r < existing_rows; r++) {
                buffer_i32_push(buf, 0);
            }
            table->column_buffers[idx] = buf;
        } else {
            raw_string_column_t *str_col = raw_string_column_create(existing_rows > 128 ? existing_rows * 2 : 128);
            // Pad existing in-memory rows with default ""
            for (size_t r = 0; r < existing_rows; r++) {
                str_col->strings[r] = strdup("");
            }
            str_col->size = existing_rows;
            table->column_buffers[idx] = str_col;
        }
    }
    return idx;
}

static void repl_exec_bulk_insert(table_t *db, const char *file_path) {
    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        printf("Error: failed to open file '%s'.\n", file_path);
        return;
    }
    // 1. Schema inference pre-scan
    char *line = NULL;
    size_t cap = 0;
    for (int i = 0; i < SCHEMA_SAMPLE_ROWS; i++) {
        ssize_t len = getline(&line, &cap, fp);
        if (len == -1)
            break; // Reached EOF before sample limit

        // Strip trailing CR/LF
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0)
            continue;

        yyjson_doc *peek_doc = yyjson_read(line, (size_t)len, 0);
        if (peek_doc) {
            yyjson_val *root = yyjson_doc_get_root(peek_doc);
            if (yyjson_is_obj(root)) {
                size_t idx, max;
                yyjson_val *k, *v;
                yyjson_obj_foreach(root, idx, max, k, v) {
                    column_type_t t = (v && yyjson_is_int(v)) ? COL_TYPE_I32 : COL_TYPE_STRING;
                    table_ensure_column(db, yyjson_get_str(k), t);
                }
            }
            yyjson_doc_free(peek_doc);
        }
    }
    rewind(fp); // Reset file offset back to beginning

    if (!db->schema || db->schema->count == 0) {
        printf("Error: could not infer schema from '%s'.\n", file_path);
        fclose(fp);
        free(line);
        return;
    }

    // 2. Prepare bounded chunk buffers
    size_t num_cols = db->schema->count;
    void **col_data = calloc(num_cols, sizeof(void *));

    for (size_t c = 0; c < num_cols; c++) {
        if (db->schema->columns[c].type == COL_TYPE_I32) {
            col_data[c] = malloc(sizeof(int32_t) * BULK_CHUNK_SIZE);
        } else {
            col_data[c] = malloc(sizeof(char *) * BULK_CHUNK_SIZE);
        }
    }

    size_t chunk_rows = 0;
    while (true) {
        ssize_t len = getline(&line, &cap, fp);
        if (len == -1)
            break;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0)
            continue;

        yyjson_doc *peek_doc = yyjson_read(line, (size_t)len, 0);
        if (peek_doc) {
            yyjson_val *root = yyjson_doc_get_root(peek_doc);
            if (yyjson_is_obj(root)) {
                size_t idx, max;
                yyjson_val *k, *v;

                // Reset row slots
                for (size_t c = 0; c < num_cols; c++) {
                    if (db->schema->columns[c].type == COL_TYPE_I32) {
                        ((int32_t *)col_data[c])[chunk_rows] = 0;
                    } else {
                        ((char **)col_data[c])[chunk_rows] = NULL;
                    }
                }

                yyjson_obj_foreach(root, idx, max, k, v) {
                    int32_t c = schema_registry_get_column_index(db->schema, yyjson_get_str(k));
                    if (c < 0)
                        continue;
                    if (db->schema->columns[c].type == COL_TYPE_I32) {
                        ((int32_t *)col_data[c])[chunk_rows] = yyjson_is_int(v) ? (int32_t)yyjson_get_int(v) : 0;
                    } else {
                        const char *s = yyjson_is_str(v) ? yyjson_get_str(v) : "";
                        ((char **)col_data[c])[chunk_rows] = strdup(s);
                    }
                }
                for (size_t c = 0; c < num_cols; c++) {
                    if (db->schema->columns[c].type != COL_TYPE_I32) {
                        char **arr = (char **)col_data[c];
                        if (!arr[chunk_rows]) arr[chunk_rows] = strdup("");
                    }
                }
                chunk_rows++;
            }
            yyjson_doc_free(peek_doc);
        }

        // Flush full chunk
        if (chunk_rows == BULK_CHUNK_SIZE) {
            table_insert_batch(db, (const void ***)col_data, num_cols, chunk_rows);
            for (size_t c = 0; c < num_cols; c++) {
                if (db->schema->columns[c].type != COL_TYPE_I32) {
                    char **arr = (char **)col_data[c];
                    for (size_t r = 0; r < chunk_rows; r++) free(arr[r]);
                }
            }
            chunk_rows = 0;
        }
    }

    // Flush incomplete tail chunk
    if (chunk_rows > 0) {
        table_insert_batch(db, (const void ***)col_data, num_cols, chunk_rows);
        for (size_t c = 0; c < num_cols; c++) {
            if (db->schema->columns[c].type != COL_TYPE_I32) {
                char **arr = (char **)col_data[c];
                for (size_t r = 0; r < chunk_rows; r++) free(arr[r]);
            }
        }
    }

    // Cleanup
    free(line);
    fclose(fp);
    for (size_t c = 0; c < num_cols; c++) free(col_data[c]);
    free(col_data);
}

void repl_exec_insert(table_t *db, ast_query_t *ast) {
    if (ast->is_bulk_insert) {
        repl_exec_bulk_insert(db, ast->from_file_path);
        return;
    }

    size_t max_cols = (db->schema ? db->schema->count : 0) + ast->num_insert_cols;
    if (max_cols == 0) return;

    int32_t *i32_vals = calloc(max_cols, sizeof(int32_t));
    const char **str_vals = calloc(max_cols, sizeof(char *));
    const void **col_ptrs = calloc(max_cols, sizeof(void *));
    for (size_t c = 0; c < max_cols; c++) {
        str_vals[c] = "";
    }

    for (size_t k = 0; k < ast->num_insert_cols; k++) {
        column_type_t type = ast->insert_vals[k].is_string ? COL_TYPE_STRING : COL_TYPE_I32;
        int32_t idx = table_ensure_column(db, ast->insert_cols[k], type);
        if (idx < 0) continue;
        if (type == COL_TYPE_I32) {
            i32_vals[idx] = ast->insert_vals[k].int_val;
        } else {
            str_vals[idx] = ast->insert_vals[k].string_val;
        }
    }

    for (size_t c = 0; c < db->schema->count; c++) {
        if (db->schema->columns[c].type == COL_TYPE_I32) {
            col_ptrs[c] = &i32_vals[c];
        } else {
            col_ptrs[c] = &str_vals[c];
        }
    }

    int rc = table_insert_batch(db, (const void ***)col_ptrs, db->schema->count, 1);
    if (rc == 0) {
        printf("INSERT 1 (table now has %zu rows in RAM).\n", db->row_count);
    } else {
        printf("Error: insert failed.\n");
    }

    free(i32_vals);
    free(str_vals);
    free(col_ptrs);
}

void repl_exec_select(table_t *db, ast_query_t *ast) {
    // Implemented in Day 22: The REPL Read Path & Query Planner
    (void)db;
    (void)ast;
}

void repl_run(table_t *db) {
    if (!db)
        return;

    char *line = NULL;
    size_t cap = 0;

    while (true) {
        printf("db> ");
        fflush(stdout);

        ssize_t num = getline(&line, &cap, stdin);
        if (num == -1) break;

        while (num > 0 && (line[num - 1] == '\n' || line[num - 1] == '\r')) {
            line[--num] = '\0';
        }

        if (num == 0)
            continue;

        if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
            break;
        }

        if (strcmp(line, ".flush") == 0) {
            table_flush(db, ".");
            printf("OK (flushed to disk part).\n");
            continue;
        }

        sql_parser_t parser = sql_parser_create(line);
        ast_query_t ast = {0};
        if (!sql_parser_parse(&parser, &ast)) {
            printf("Error: invalid SQL syntax.\n");
            continue;
        }
        if (ast.stmt_type == STMT_INSERT) {
            repl_exec_insert(db, &ast);
        } else if (ast.stmt_type == STMT_SELECT) {
            repl_exec_select(db, &ast);
        }
    }

    free(line);
}
```

---

## 9. Next Up: Day 22 — The REPL Read Path & Query Planner

With the write path, dynamic schema evolution, and streaming NDJSON ingestion complete, **Day 22** will build `repl_exec_select()`—our **Mini Query Planner** that binds logical `SELECT ... WHERE ... GROUP BY ...` ASTs into Day 18's vectorized physical execution pipeline!

---

> **Day 21 Branch**: View the exact code for this day at [dmanchon/simpledb (`day21`)](https://github.com/dmanchon/simpledb/tree/day21) ([diff from `day20`](https://github.com/dmanchon/simpledb/compare/day20...day21)).
