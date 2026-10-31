# Day 22: The REPL Read Path & Query Planner (Binding SQL `SELECT` to the Vectorized Pipeline)

> **Day 22 Branch**: View the exact code for this day at [dmanchon/simpledb (`day22`)](https://github.com/dmanchon/simpledb/tree/day22) ([diff from `day21`](https://github.com/dmanchon/simpledb/compare/day21...day22)).

> [!TIP]
> **Long-Read Engineering Log — TL;DR & Fast Navigation**
> This post is an unabridged Socratic pair-programming log capturing the architecture, C11 pointer mechanics, and paging edge cases encountered while building the REPL Read Path and Query Planner.
> - **Bottom-Up Tree Accumulator (`repl_exec_select`):** Translates Day 20's logical `ast_query_t` into a physical operator tree (`OP_SCAN` $\to$ `OP_FILTER` $\to$ `OP_AGGREGATE`) and returns a `physical_operator_t *` cursor (`sqlite3_prepare` / `sqlite3_step` pattern).
> - **Projected Block Index vs. Table Schema Index:** Resolves `WHERE` and `GROUP BY` column names against the projected `column_names[]` array rather than the raw table schema to prevent out-of-bounds access on `block.columns[]`.
> - **Three Multi-Page Execution Bug Fixes (`src/exec_pipeline.c`):** Handles empty filtered granules (`sel->size == 0`) without premature `EOF`, guards `OP_AGGREGATE` ingestion across multi-page output streams (`finished_ingestion`), and fixes the C11 short-circuit `&&` operand order at page boundary `8,192`.
> - **Want to go straight to the code?** [Jump to the Multi-Page Unit Tests](#5-unit-testing-the-read-path-teststest_dbc) or [Jump to the Final C11 Implementation (`src/repl.c` & `src/exec_pipeline.c`)](#8-the-final-listing-srcreplc--srcexec_pipelinec).

Welcome to Day 22! In Day 21, we completed the interactive REPL loop and the **Write Path**—enabling dynamic schema evolution (`table_ensure_column`), single-row point inserts (`repl_exec_insert`), and two-phase streaming NDJSON ingestion (`repl_exec_bulk_insert`) with `yyjson`.

Today, we complete the **Read Path** (`repl_exec_select`) by building our **Mini Query Planner and Binder**. When a user enters an analytical `SELECT` query in the REPL:

```sql
SELECT user_id, SUM(amount) FROM events WHERE amount > 100 GROUP BY user_id;
```

`repl_exec_select()` must bridge **Day 20's Logical AST (`ast_query_t`)** with **Day 18's Vectorized Physical Execution Pipeline (`physical_operator_t`)**!

---

## 1. Architecture: From Logical AST to Physical Operator DAG

In a database engine, the parser only understands *syntax*—it produces an `ast_query_t` containing raw column name strings (`"user_id"`, `"amount"`). The physical execution engine (`exec_pipeline_run`), on the other hand, operates on **contiguous columnar arrays and integer column indices (`size_t column_idx`)** inside a `block_t`.

Translating an `ast_query_t` into a runnable `physical_operator_t` tree requires four stages:

```
                  ast_query_t (Day 20 Logical AST)
   SELECT user_id, SUM(amount) FROM events WHERE amount > 100 GROUP BY user_id
                                 │
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │ 1. Column Projection Collection & Binding                 │
   │    Collect unique column names referenced across          │
   │    SELECT, WHERE, and GROUP BY -> cols[] = {"user_id",    │
   │    "amount"} and map each name to its scan block index.   │
   └─────────────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │ 2. Leaf Operator: OP_SCAN                                 │
   │    table_scanner_create(db, cols, num_cols)               │
   └─────────────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │ 3. Predicate Pushdown / Filter Operator: OP_FILTER        │
   │    Wraps OP_SCAN for each ast->predicates[p]              │
   │    Maps OP_GREATER_THAN -> COMPARE_GT, threshold = 100    │
   └─────────────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │ 4. Root Aggregation Operator: OP_AGGREGATE                │
   │    If ast->has_group_by: wraps OP_FILTER with             │
   │    grace_hash_agg_create("/tmp", 1024, 4)                 │
   └─────────────────────────────┬─────────────────────────────┘
                                 │
                                 ▼
                  block_t result = exec_pipeline_run(root)
```

---

## 2. The Column Binding Problem: Table Schema Index vs. Scan Block Index

A subtle architectural distinction arises when building the physical operator tree:

- **Table Schema Index (`db->schema`):** In the table's full schema (for example, our NDJSON bulk ingestion table from Day 21), `"user_id"` might be column `0`, `"status"` column `1`, `"amount"` column `2`, and `"region"` column `3`.
- **Projected Scan Block Index (`block_t`):** Because Day 17's [`table_scanner_create(db, cols, num_cols)`](file:///Users/dmanchon/repos/cdb/src/table_scanner.c#L11-L25) only loads the *requested* columns (`cols`), if the query only touches `{"user_id", "amount"}`, the `block_t` emitted by `OP_SCAN` has only **2 columns**:
  - `block.columns[0]` $\rightarrow$ `"user_id"` (scan index `0`)
  - `block.columns[1]` $\rightarrow$ `"amount"` (scan index `1`)

> [!WARNING]
> **Watch Out for Memory Buffer Indexing in `table_scanner_next`:**
> Look closely at [`src/table_scanner.c`](file:///Users/dmanchon/repos/cdb/src/table_scanner.c#L81-L85) when scanning the active RAM `column_buffers`:
> - For on-disk parts (lines 36–43), `table_scanner_next` looks up `col_idx = schema_registry_get_column_index(scanner->table->schema, scanner->column_names[i])`.
> - For the in-memory buffer (line 82), it currently indexes `scanner->table->column_buffers[i]` directly instead of resolving `col_idx` via `schema_registry_get_column_index()`!
> - If `"amount"` is at schema index `2` (because `"status"` is at index `1`), `column_buffers[1]` is actually a `raw_string_column_t*` (`"status"`), not `"amount"`, unless `table_scanner_next` resolves `col_idx = schema_registry_get_column_index(scanner->table->schema, scanner->column_names[i])` for memory buffers just as it does for disk parts!

### Step-by-Step Anatomy of `repl_exec_select(table_t *db, ast_query_t *ast)`

How do you programmatically build a tree of `physical_operator_t` nodes when clauses like `WHERE` and `GROUP BY` are optional?

In database query planners, the standard technique is the **Bottom-Up Tree Accumulator (`root` pointer wrapping)** combined with an idempotent **`ensure_scan_col()`** helper:

#### 1. The Idempotent Scan Column Binder (`ensure_scan_col`)
Instead of separately deduplicating column names and searching for their indices later, a single 8-line helper does both:
```c
static size_t ensure_scan_col(const char **cols, size_t *num_cols, const char *name) {
    for (size_t i = 0; i < *num_cols; i++) {
        if (strcmp(cols[i], name) == 0) return i;
    }
    size_t idx = (*num_cols)++;
    cols[idx] = name;
    return idx;
}
```
- Before creating `OP_SCAN`, call `ensure_scan_col(cols, &num_cols, ...)` for every column in `ast->select_exprs`, `ast->predicates`, and `ast->group_by_col`.
- Later, when configuring `OP_FILTER` or `OP_AGGREGATE`, calling `ensure_scan_col(cols, &num_cols, col_name)` again returns the exact `0 .. num_cols - 1` slot inside the `block_t` emitted by `OP_SCAN`!

#### 2. The Bottom-Up `root` Accumulator Pattern
You maintain a single pointer `physical_operator_t *root` that starts at the leaf (`OP_SCAN`) and gets wrapped layer-by-layer whenever an AST clause is present:

```
   Stage 1 (Always):
   root ──► [ OP_SCAN ]

   Stage 2 (If ast->num_predicates > 0, wrap root):
   filter_op->children[0] = root;
   root = filter_op;
   root ──► [ OP_FILTER ] ──► [ OP_SCAN ]

   Stage 3 (If ast->has_group_by, wrap root again):
   agg_op->children[0] = root;
   root = agg_op;
   root ──► [ OP_AGGREGATE ] ──► [ OP_FILTER ] ──► [ OP_SCAN ]
```

Because each optional stage simply wraps `children[0] = root` and reassigns `root = new_op`, `STAGE 3` doesn't need to know whether `WHERE` was present or not—`root` is *always* the valid upstream operator!

> [!CAUTION]
> **Why `calloc(1, sizeof(physical_operator_t))` Is Mandatory for Leaf Nodes:**
> Look at [`physical_operator_destroy()`](file:///Users/dmanchon/repos/cdb/src/exec_pipeline.c#L80-L89) in `src/exec_pipeline.c`:
> ```c
> void physical_operator_destroy(physical_operator_t *op) {
>     if (!op) return;
>     for (size_t i = 0; i < op->num_children; i++) {
>         physical_operator_destroy(op->children[i]);
>     }
>     if (op->children) free(op->children);
>     free(op);
> }
> ```
> If you allocate `op_scan` using `malloc(sizeof(physical_operator_t))` without explicitly setting `op_scan->num_children = 0;` and `op_scan->children = NULL;`, those fields contain uninitialized heap garbage and `physical_operator_destroy()` will dereference a wild pointer! Using `calloc(1, sizeof(physical_operator_t))` zeroes `num_children` and `children` automatically.

#### 3. Translating AST Enums to Physical Enums & Avoiding Out-of-Scope / Out-of-Bounds Traps
When wrapping `OP_FILTER` inside `if (ast->num_predicates > 0)`, watch out for three classic C pitfalls:

1. **Keep `physical_operator_t *root = op_scan;` Outside the `if` Block:**
   If `physical_operator_t *op_filter` is declared *inside* `if (ast->num_predicates > 0) { ... }` without reassigning an outer `root = op_filter;` pointer, `op_filter` goes out of scope as soon as the `if` block closes! Declaring `physical_operator_t *root = op_scan;` before the `if` block and setting `root = op_filter;` at the end of the `if` block keeps the top of the tree accessible to the `GROUP BY` stage and `exec_pipeline_run(root)`.
2. **Search `column_names[]` (`0 .. ast->num_select_exprs`), NOT `db->schema->columns[]`:**
   Because `table_scanner_create(db, (const char **)column_names, ast->num_select_exprs)` produces a `block_t` with `ast->num_select_exprs` columns, `block.columns[]` is indexed from `0` to `ast->num_select_exprs - 1`.
   - If a table has 4 columns in `db->schema` (`0: user_id`, `1: status`, `2: amount`, `3: region`) and the query is `SELECT user_id, SUM(amount) ... WHERE amount < 100`, `block.columns` only has **2 elements** (`0: user_id`, `1: amount`).
   - Looking up `"amount"` in `db->schema` returns `2`, which would index `block.columns[2]` **out of bounds**!
   - Looking up `"amount"` in `column_names` (`for (size_t i = 0; i < ast->num_select_exprs; i++)`) returns `1`, matching `block.columns[1]`:
   ```c
   for (size_t i = 0; i < ast->num_select_exprs; i++) {
       if (strcmp(ast->predicates[0].column_name, column_names[i]) == 0) {
           column_id = (int)i;
           break;
       }
   }
   ```
3. **Cover All Three `ast_op_t` Enum Cases in `switch`:**
   Always include `OP_EQUALS`, `OP_LESS_THAN`, and `OP_GREATER_THAN` (each with a `break;`) so `-Wswitch` does not fail compilation:
   ```c
   switch (ast->predicates[0].op) {
   case OP_EQUALS:
       op_filter->filter.op = COMPARE_EQ;
       break;
   case OP_LESS_THAN:
       op_filter->filter.op = COMPARE_LT;
       break;
   case OP_GREATER_THAN:
       op_filter->filter.op = COMPARE_GT;
       break;
   }
   ```

---

## 3. Architectural Deep-Dive: Why Current `WHERE` Is Single-Predicate & How to Add Boolean `AND` / `OR`

Right now, `simpledb` only supports a single `WHERE` predicate (`ast->num_predicates` is `0` or `1`), for two reasons:
1. **Parser Grammar (`src/sql_parser.c`):** Even though `ast_query_t` reserves `ast_predicate_t predicates[16]`, [`sql_parser_parse_select()`](file:///Users/dmanchon/repos/cdb/src/sql_parser.c#L278-L304) uses a single `if (parser->current_token.type == TOKEN_WHERE)` block and does not tokenize `AND` or `OR`.
2. **Physical Execution (`src/exec_pipeline.c`):** Look at `case OP_FILTER:` in [`exec_pipeline_run()`](file:///Users/dmanchon/repos/cdb/src/exec_pipeline.c#L54-L76):
   ```c
   case OP_FILTER: {
       block_t block = exec_pipeline_run(root->children[0]);
       if (block.num_rows == 0) return block;

       selection_vector_t *sel = selection_vector_create(block.num_rows);
       // Runs filter_*_i32 over raw block.columns[col_idx] from 0 .. len-1
       ...
       block.num_rows = sel->size;
       block.sel_vector = sel;
       return block;
   }
   ```
   Notice that `OP_FILTER` ignores any incoming `block.sel_vector` and overwrites `block.sel_vector = sel`! If two `OP_FILTER` nodes were stacked in a chain (`OP_FILTER_2 -> OP_FILTER_1 -> OP_SCAN`), the upper filter would discard the lower filter's selection vector.

### How Difficult Would It Be to Add Boolean Operators (`AND` / `OR`)?

A natural first thought is: *Why not add an `OP_BOOL` physical operator node with `children = [exp1, exp2, ...]`?*

#### The Trap: Pipeline Operator Tree vs. Expression Tree (The Double-Scan Bug)
In a pull-based Volcano/vectorized pipeline (`exec_pipeline_run`), every `physical_operator_t` node is a **data stream iterator** that pulls a `block_t` from `children[0]`:

```
          ❌ BROKEN: OP_BOOL as a Pipeline Operator
                        [ OP_BOOL (AND) ]
                         /             \
           [ OP_FILTER (a > 10) ]   [ OP_FILTER (b < 50) ]
                         \             /
                           [ OP_SCAN ]
```
If `OP_BOOL` calls `exec_pipeline_run(children[0])` and then `exec_pipeline_run(children[1])`:
- `children[0]` calls `table_scanner_next()` and pulls **Batch 1** (rows `0..8191`).
- `children[1]` calls `table_scanner_next()` and pulls **Batch 2** (rows `8192..16383`)!
Because `OP_SCAN` is stateful, branching the physical pipeline causes each branch to consume *different* granules from the scanner!

#### The Solution: Evaluate Boolean `AND` / `OR` Over a Single `block_t`

In ClickHouse and DuckDB, `OP_FILTER` remains a **single-child pipeline operator** (`OP_FILTER -> OP_SCAN`) that pulls one `block_t` at a time, and evaluates a **Boolean Predicate Tree (`filter_expr_t`)** against that same `block_t`:

```
          ✅ CORRECT: Single Pipeline Stream + Expression Tree Inside OP_FILTER
                        [ OP_FILTER ]  ---> Evaluates: BOOL_AND(a > 10, b < 50)
                              │             on the SAME block_t!
                         [ OP_SCAN ]
```

Because both `filter_gt_i32` and `filter_lt_i32` produce sorted `selection_vector_t`s (or Day 6 `bitmap_t` bitmasks), adding `AND` / `OR` requires only **~60 lines of C** across three small changes:

| Layer | What Changes | Complexity |
| :--- | :--- | :--- |
| **1. Lexer & Parser (`sql_parser.h/.c`)** | Add `TOKEN_AND`, `TOKEN_OR`. Change `if (TOKEN_WHERE)` to a `while` loop that parses predicates while `current_token` is `TOKEN_AND` or `TOKEN_OR`, storing `bool_op_t` (`BOOL_AND` / `BOOL_OR`) in `ast_query_t`. | ~20 LOC |
| **2. Selection Vector Set Algebra (`filter.c`)** | Because `selection_vector_t` stores strictly increasing row indices (`e.g. [1, 4, 9]` and `[4, 7, 9]`), two selection vectors can be combined using a two-pointer merge in $O(N)$ time:<br>• **`AND` (Intersection):** Emit indices present in *both* `sel_a` and `sel_b` (`[4, 9]`).<br>• **`OR` (Union):** Emit deduplicated indices present in *either* `sel_a` or `sel_b` (`[1, 4, 7, 9]`).<br>*(Alternatively, evaluate each predicate into a Day 6 `bitmap_t` and use `bitmap_and()` / `bitmap_or()`!)*. | ~25 LOC |
| **3. Filter Operator (`exec_pipeline.h/.c`)** | Allow `OP_FILTER` (or stacked `OP_FILTER` nodes) to intersect/union the newly computed `sel` with the incoming `block.sel_vector` instead of overwriting it! | ~15 LOC |

Even simpler: if you only need **`AND` chains** (`WHERE a > 10 AND b < 50`), you don't even need a separate `OP_BOOL` node! In `OP_FILTER`, if `block.sel_vector != NULL` (from a child `OP_FILTER`), intersecting the new `sel` with `block.sel_vector` (and freeing the old `block.sel_vector`) automatically makes any chain of `OP_FILTER -> OP_FILTER -> OP_SCAN` act as a logical **`AND`**!

#### 4. Returning the Pipeline (`physical_operator_t *`) — The `sqlite3_prepare` / `sqlite3_step` Cursor Pattern

Why should `repl_exec_select(table_t *db, ast_query_t *ast)` return **`physical_operator_t *`** instead of `void` or a single `block_t`?

1. **Decoupling Planning from Paged Execution:**
   - If `repl_exec_select()` returned `void` and only `printf`d inside an internal loop, unit tests could not inspect the resulting `block_t` slices and `assert()` on their values.
   - If `repl_exec_select()` returned a single `block_t`, the caller could only ever read the first page (8,192 rows) of a multi-granule scan or high-cardinality aggregation, and the `physical_operator_t *root` tree would be leaked!
2. **Clean Ownership & Cursor Exhaustion (Like `sqlite3_prepare_v2` + `sqlite3_step`):**
   - `repl_exec_select(table_t *db, ast_query_t *ast)` acts strictly as the **Query Planner & Binder**: it translates `ast_query_t` into a `physical_operator_t *root` pipeline (`OP_SCAN` $\to$ `OP_FILTER` $\to$ `OP_AGGREGATE`) and returns `root` (or `NULL` on error).
   - Whoever calls `repl_exec_select()`—whether `repl_run()` in the interactive CLI or `test_repl_read_path_execution()` in the test suite—owns the pipeline cursor, pulls pages via `exec_pipeline_run(pipeline)` until `block.num_rows == 0`, and then destroys the pipeline via `physical_operator_destroy(pipeline)`:

```c
physical_operator_t *pipeline = repl_exec_select(db, &ast);
if (pipeline) {
    while (true) {
        block_t block = exec_pipeline_run(pipeline);
        if (block.num_rows == 0) break;
        // In REPL: print each row in block
        // In Unit Test: assert() on block rows and columns
    }
    physical_operator_destroy(pipeline);
}
```

> [!CAUTION]
> **Three Subtle Paging Bugs Caught by Live REPL & Multi-Block Testing (`src/exec_pipeline.c`):**
>
> 1. **`OP_FILTER` Confusing an Empty Filtered Part/Granule (`sel->size == 0`) with Stream `EOF` (`block.num_rows == 0`):**
>    Suppose Part 0 (`./all_1_1_0`) contains 1 row `(2, 50)` and Part 1 (`./all_2_2_0`) contains `(1, 150), (1, 250), (3, 300), (2, 180)`. When the user runs:
>    ```sql
>    SELECT user_id, amount FROM events WHERE amount > 149;
>    ```
>    - `OP_SCAN` first emits Part 0 (`(2, 50)`).
>    - `OP_FILTER` evaluates `amount > 149` on Part 0 and finds **0 matching rows (`sel->size == 0`)**.
>    - If `OP_FILTER` simply sets `block.num_rows = sel->size;` (`0`) and returns `block`, the downstream caller (`repl_run` or `OP_AGGREGATE`) sees `block.num_rows == 0`—which is the sentinel for **End-Of-Stream (EOF)**—and terminates the query immediately without ever scanning Part 1!
>    - **The Fix:** `case OP_FILTER:` must loop (`while (1)`) until either the upstream child genuinely returns EOF (`block.num_rows == 0`) or at least 1 row matches the filter (`sel->size > 0`):
>    ```c
>    if (sel->size == 0) {
>        selection_vector_free(sel);
>        free(block.columns);
>        continue; // 0 rows matched in this granule/part; pull next upstream block!
>    }
>    ```
>
> 2. **Guarding `OP_AGGREGATE` Ingestion Across Multiple Paged Calls (`finished_ingestion`):**
>    Look at `case OP_AGGREGATE:` in [`src/exec_pipeline.c`](file:///Users/dmanchon/repos/cdb/src/exec_pipeline.c#L13-L55). When a caller exhausts `pipeline` in a `while (true)` loop across 15,000 groups, `exec_pipeline_run(pipeline)` is called three times (Page 1: `8,192` rows, Page 2: `6,808` rows, Page 3: `0` rows).
>    Wrapping the upstream ingestion loop and `grace_hash_agg_finish()` inside `if (!root->aggregate.aggregator->finished_ingestion)` ensures Page 1 ingests and finalizes the partitions once, while Page 2 and Page 3 stream the remaining groups from `grace_hash_agg_next()`.
>
> 3. **Short-Circuit `&&` Operand Order (`out_block.num_rows < 8192` Must Come First!):**
>    Look at how the output loop in `src/exec_pipeline.c` was originally written on Day 18:
>    ```c
>    // ❌ BUG: Drops record #8192 at the page boundary!
>    while (grace_hash_agg_next(root->aggregate.aggregator, &out_rec) &&
>           out_block.num_rows < 8192)
>    ```
>    In C, the logical `&&` operator evaluates its **left operand first**. When `out_block.num_rows` reaches `8192`:
>    - Because `grace_hash_agg_next(...)` was on the left side of `&&`, C called `grace_hash_agg_next()` **first**—advancing `hash_table_iter_idx++` and popping the 8,193rd group into `out_rec`—and **then** evaluated `out_block.num_rows < 8192` (`false`), exiting the loop and **dropping the 8,193rd record on the floor** (`8192 + 6807 = 14999` instead of `15000`)!
>    - Flipping the operand order so the bounds check is on the left:
>    ```c
>    // ✅ CORRECT: Short-circuits before calling grace_hash_agg_next() when page is full!
>    while (out_block.num_rows < 8192 &&
>           grace_hash_agg_next(root->aggregate.aggregator, &out_rec))
>    ```
>    short-circuits as soon as `out_block.num_rows == 8192`, preserving the 8,193rd group for Page 2 so `8192 + 6808 = 15000`!

---

## 4. Header Interfaces (`include/simpledb/repl.h` & `include/simpledb/exec_pipeline.h`)

Our Query Planner connects the AST structures in `sql_parser.h` to the physical operators in `exec_pipeline.h`:

```c
// From include/simpledb/repl.h
#pragma once
#include "simpledb/exec_pipeline.h"
#include "simpledb/sql_parser.h"
#include "simpledb/table.h"

void repl_run(table_t *db);
void repl_exec_insert(table_t *db, ast_query_t *ast);
physical_operator_t *repl_exec_select(table_t *db, ast_query_t *ast);
```

And the physical operator nodes from `include/simpledb/exec_pipeline.h`:

```c
// From include/simpledb/exec_pipeline.h
typedef enum {
    OP_SCAN,
    OP_FILTER,
    OP_AGGREGATE
} operator_type_t;

struct physical_operator_t {
    operator_type_t type;
    physical_operator_t** children;
    size_t num_children;
    union {
        scan_operator_t scan;
        filter_operator_t filter;
        aggregate_operator_t aggregate;
    };
};

block_t exec_pipeline_run(physical_operator_t* root);
void physical_operator_destroy(physical_operator_t* op);
```

---

## 5. Unit Testing the Read Path (`tests/test_db.c`)

When writing `test_repl_read_path_execution()` in [`tests/test_db.c`](file:///Users/dmanchon/repos/cdb/tests/test_db.c#L1321-L1360), watch out for three important interactions with our existing parser and schema layout:

1. **Supported SQL Grammar in Day 20 (`sql_parser.c`):**
   - Our Day 20 lexer and parser ([`src/sql_parser.c`](file:///Users/dmanchon/repos/cdb/src/sql_parser.c#L221-L339)) support explicit column projections and aggregations (`SELECT user_id, amount ...` or `SELECT user_id, SUM(amount) ...`), `WHERE`, `GROUP BY`, and `ORDER BY`.
   - It does **not** tokenize `*` (`SELECT *` produces `TOKEN_ERROR` and causes `sql_parser_parse()` to return `false`) or `LIMIT`. Always wrap `assert(sql_parser_parse(&parser, &ast));` in tests so syntax failures are caught immediately!
2. **Matching Assertions to the NDJSON Schema:**
   - If the NDJSON loop writes `{"user_id": i, "amount": 99, "region": "EU"}` for `i = 0 .. 14999` (without the initial `"status": "PENDING"` row), then:
     - `table->column_buffers[0]` is `user_id` (`COL_TYPE_I32`, starting at `0`, not `101`).
     - `table->column_buffers[1]` is `amount` (`COL_TYPE_I32`), **not** a `raw_string_column_t`! Casting `column_buffers[1]` to `raw_string_column_t *` when `status` was never inserted will reinterpret a `buffer_i32_t` as a string column and segfault.
3. **Registering Both Test Functions in `main()`:**
   - After splitting `test_repl_execution()` into `test_repl_write_path_execution()` and `test_repl_read_path_execution()`, both functions must be called in `main()`.

Here is the complete `test_repl_read_path_execution()` suite verifying **three multi-page (> 1 block) scenarios**:
1. **In-Memory Multi-Page Aggregation (`2` blocks):** 15,000 distinct `user_id` groups paged out across Page 1 (`8,192` rows) and Page 2 (`6,808` rows).
2. **Hybrid Disk-Part + Active-RAM Unaggregated Filter Scan (`3` blocks):** Flushing the 15,000 rows to disk (`./all_1_1_0` with 2 granules of `8,192` and `6,808` rows) + inserting `5,000` new rows into active RAM, then streaming `SELECT user_id, amount FROM events WHERE amount < 100;` across **3 `block_t` pages** (`20,000` rows total).
3. **Hybrid Disk-Part + Active-RAM Selective `WHERE` + `GROUP BY` (`2` blocks):** Filtering `WHERE amount > 0` across the 20,000 hybrid rows (eliminating the `4,000` rows where `i % 5 == 0`) and aggregating the remaining **`16,000` groups** across 2 output pages (`8,192` + `7,808` rows), verifying `sum == (uid % 5)` on every row:

```c
static void test_repl_read_path_execution(void) {
    table_t *table = table_create("events_merge_test");

    {
        // 1. Multi-page in-memory GROUP BY (2 blocks: 8192 + 6808 = 15,000 groups)
        char tmp_path[] = "/tmp/cdb_bulk_XXXXXX";
        int fd = mkstemp(tmp_path);
        assert(fd != -1);
        FILE *f = fdopen(fd, "w");
        assert(f != NULL);

        for (int i = 0; i < 15000; i++) {
            fprintf(f, "{\"user_id\": %d, \"amount\": %d, \"region\": \"EU\"}\n", i, i % 5);
        }
        fclose(f); // Closes both FILE* and the underlying fd
        char query[256];
        snprintf(query, sizeof(query), "INSERT INTO events FROM '%s';", tmp_path);

        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
        assert(table->row_count == 15000);
        assert(table->schema != NULL);
        assert(table->schema->count == 3);
        // Verify physical columnar vectors in memory
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[0] == 0);

        const char select_query[256] =
            "SELECT user_id, SUM(amount) FROM events WHERE amount < 100 GROUP BY user_id;";
        parser = sql_parser_create(select_query);
        assert(sql_parser_parse(&parser, &ast));
        physical_operator_t *pipeline = repl_exec_select(table, &ast);
        assert(pipeline != NULL);

        size_t total_rows = 0;
        size_t num_blocks = 0;
        while (true) {
            block_t block = exec_pipeline_run(pipeline);
            if (block.num_rows == 0)
                break;
            total_rows += block.num_rows;
            num_blocks++;
        }
        assert(num_blocks == 2); // Page 1: 8192 groups, Page 2: 6808 groups
        assert(total_rows == 15000);
        physical_operator_destroy(pipeline);
        remove(tmp_path);
    }

    // 2. Multi-page unaggregated SELECT + WHERE across Flushed Disk Granules + Active RAM Buffer (3 blocks)
    {
        // Flush the 15,000 in-memory rows to disk part ./all_1_1_0 (2 granules: 8192 + 6808 rows)
        assert(table_flush(table, ".") == 0);
        assert(table->num_parts == 1);
        assert(table->row_count == 0);

        // Insert 5,000 additional rows into active RAM buffer (user_id: 15000..19999)
        char tmp_path2[] = "/tmp/cdb_bulk2_XXXXXX";
        int fd2 = mkstemp(tmp_path2);
        assert(fd2 != -1);
        FILE *f2 = fdopen(fd2, "w");
        assert(f2 != NULL);
        for (int i = 15000; i < 20000; i++) {
            fprintf(f2, "{\"user_id\": %d, \"amount\": %d, \"region\": \"EU\"}\n", i, i % 5);
        }
        fclose(f2);

        char insert_q2[256];
        snprintf(insert_q2, sizeof(insert_q2), "INSERT INTO events FROM '%s';", tmp_path2);
        sql_parser_t parser = sql_parser_create(insert_q2);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
        assert(table->row_count == 5000);
        remove(tmp_path2);

        // Query without GROUP BY: streams 2 disk granule blocks (8192 + 6808) + 1 RAM block (5000) = 3 blocks
        const char *scan_filter_q = "SELECT user_id, amount FROM events WHERE amount < 100;";
        parser = sql_parser_create(scan_filter_q);
        assert(sql_parser_parse(&parser, &ast));
        physical_operator_t *pipeline = repl_exec_select(table, &ast);
        assert(pipeline != NULL);

        size_t total_rows = 0;
        size_t num_blocks = 0;
        while (true) {
            block_t block = exec_pipeline_run(pipeline);
            if (block.num_rows == 0)
                break;
            total_rows += block.num_rows;
            num_blocks++;
        }
        assert(num_blocks == 3); // Block 1: 8192 (disk), Block 2: 6808 (disk), Block 3: 5000 (RAM)
        assert(total_rows == 20000);
        physical_operator_destroy(pipeline);
    }

    // 3. Multi-page selective WHERE (amount > 0) + GROUP BY across Disk + RAM (2 blocks, 16,000 groups)
    {
        const char *agg_filter_q =
            "SELECT user_id, SUM(amount) FROM events WHERE amount > 0 GROUP BY user_id;";
        sql_parser_t parser = sql_parser_create(agg_filter_q);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        physical_operator_t *pipeline = repl_exec_select(table, &ast);
        assert(pipeline != NULL);

        size_t total_rows = 0;
        size_t num_blocks = 0;
        while (true) {
            block_t block = exec_pipeline_run(pipeline);
            if (block.num_rows == 0)
                break;
            for (size_t r = 0; r < block.num_rows; r++) {
                int32_t uid = block.columns[0].data[r];
                int32_t sum = block.columns[1].data[r];
                assert(sum > 0);
                assert(sum == (uid % 5));
            }
            total_rows += block.num_rows;
            num_blocks++;
        }
        // Out of 20,000 rows, 4/5 have amount > 0 -> 16,000 groups across 2 blocks (8192 + 7808)
        assert(num_blocks == 2);
        assert(total_rows == 16000);
        physical_operator_destroy(pipeline);

        remove("./all_1_1_0/user_id.bin");
        remove("./all_1_1_0/user_id.mrk");
        remove("./all_1_1_0/user_id.idx");
        remove("./all_1_1_0/amount.bin");
        remove("./all_1_1_0/amount.mrk");
        remove("./all_1_1_0/amount.idx");
        remove("./all_1_1_0/region.bin");
        remove("./all_1_1_0/region.mrk");
        remove("./all_1_1_0/region.idx");
        remove("./all_1_1_0/region.strings");
        remove("./all_1_1_0/region.offsets");
        remove("./all_1_1_0");
    }

    table_destroy(table);
    printf("Day 22 repl execution read-path runtime passed!\n");
}
```

---

## 6. Wiring Up `src/main.c` & Running the Interactive `./build/simpledb` REPL

 Look at [`CMakeLists.txt`](file:///Users/dmanchon/repos/cdb/CMakeLists.txt#L26-L29): the main executable target `simpledb` is already built from [`src/main.c`](file:///Users/dmanchon/repos/cdb/src/main.c) and linked against `simpledb_lib`.

Until now, `src/main.c` still held the Day 0 placeholder (`db_open("default")`). To launch our interactive REPL when running `./build/simpledb`, `src/main.c` simply needs to:
1. Include `"simpledb/repl.h"` and `"simpledb/table.h"`.
2. Instantiate a `table_t *table = table_create("default")` (or accept an optional table name from `argv[1]`).
3. Call `repl_run(table);` to enter the `db> ` loop.
4. Clean up via `table_destroy(table);` when the user types `.exit`, `.quit`, or presses `Ctrl+D`.

```c
// src/main.c
#include "simpledb/repl.h"
#include "simpledb/table.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("simpledb version 0.1.0 (C11 Edition)\n");
        return 0;
    }

    const char *table_name = (argc > 1) ? argv[1] : "events";
    printf("Starting simpledb C11 Columnar Database Engine (table: %s)...\n", table_name);

    table_t *table = table_create(table_name);
    if (table) {
        repl_run(table);
        table_destroy(table);
    }
    return 0;
}
```

Once `src/main.c` calls `repl_run(table)`, you can build and launch the interactive REPL directly from your terminal:

```bash
cmake --build build
./build/simpledb
```

### Interactive REPL Session Walkthrough

Try pasting these commands into `./build/simpledb` to test point inserts, schema evolution, unaggregated `SELECT`, filtered `WHERE`, `GROUP BY` aggregation, `.flush` to disk parts, and hybrid disk + RAM queries:

```sql
-- 1. Point inserts (schema inferred automatically on first insert!)
INSERT INTO events (user_id, amount) VALUES (1, 150);
INSERT INTO events (user_id, amount) VALUES (2, 50);
INSERT INTO events (user_id, amount) VALUES (1, 250);
INSERT INTO events (user_id, amount) VALUES (3, 300);
INSERT INTO events (user_id, amount) VALUES (2, 180);

-- 2. Unaggregated scan of all rows
SELECT user_id, amount FROM events;

-- 3. Filtered scan (WHERE amount > 100)
SELECT user_id, amount FROM events WHERE amount > 100;

-- 4. Filtered GROUP BY + SUM aggregation
SELECT user_id, SUM(amount) FROM events WHERE amount > 100 GROUP BY user_id;

-- 5. Flush active RAM buffers to an immutable on-disk part (./all_1_1_0)
.flush

-- 6. Insert new rows into RAM and run a hybrid Disk Part + RAM query!
INSERT INTO events (user_id, amount) VALUES (1, 500);
SELECT user_id, SUM(amount) FROM events WHERE amount > 100 GROUP BY user_id;

-- 7. Exit the REPL
.exit
```

Or to test **Bulk NDJSON Streaming Ingestion** in the REPL, first create a quick NDJSON file in `/tmp/events.json`:
```bash
printf '{"user_id": 10, "amount": 120, "region": "EU"}\n{"user_id": 20, "amount": 80, "region": "US"}\n{"user_id": 10, "amount": 300, "region": "EU"}\n' > /tmp/events.json
```
And then inside `./build/simpledb`:
```sql
INSERT INTO events FROM '/tmp/events.json';
SELECT user_id, SUM(amount) FROM events WHERE amount > 100 GROUP BY user_id;
.exit
```

---

## 7. Codebase Footprint & Beyond Day 22: The jsonbench.com Challenge

Across 22 days of building `simpledb` from scratch in C11, the entire hand-written database engine remains remarkably compact:

| Subsystem | Files | Lines of Code (LOC) |
| :--- | :--- | :--- |
| **Public C11 Headers (`include/simpledb/*.h`)** | 24 files | `1,074` LOC |
| **Core Engine Implementation (`src/*.c`, excl. vendored `yyjson`)** | 23 files | `3,130` LOC |
| **Total Core Database Engine** | **47 files** | **`4,204` LOC** |
| **End-to-End Unit Test Suite (`tests/test_db.c`)** | 1 file | `1,357` LOC |
| **Total Hand-Written C11** | **48 files** | **`5,561` LOC** |

Once `repl_exec_select()` is wired up, `simpledb` is a complete end-to-end C11 columnar analytical database! To push `simpledb` toward massive industry benchmarks such as **[jsonbench.com](https://jsonbench.com)** (1 Billion JSON events totaling ~500GB from the BlueSky firehose):

1. **JSON Path Flattening:** Deeply nested JSON (`{"data": {"commit": {"repo": "..."}}}`) can be flattened into dot-notated column names (`data.commit.repo`).
2. **SIMD Parsing (`simdjson`):** Using AVX-512 / ARM NEON vector instructions to parse gigabytes of raw JSON per second directly into columnar memory.
3. **Block-Level Compression (LZ4 / Zstandard):** Applying lightweight block compression to `.bin` columnar parts to reduce disk footprint by 4x–8x.

---

## 8. The Final Listing (`src/repl.c` & `src/exec_pipeline.c`)

Below is the complete C11 implementation of the Day 22 Read Path Query Planner (`repl_exec_select`), interactive cursor loop (`repl_run`), and the multi-page execution pipeline (`exec_pipeline_run`):

### `src/repl.c`

```c
#include "simpledb/repl.h"
#include "simpledb/schema_registry.h"
#include "simpledb/sql_parser.h"
#include "simpledb/table.h"
#include "simpledb/exec_pipeline.h"
#include "simpledb/table_scanner.h"
#include "yyjson.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
    // schema inference
    char *line = NULL;
    size_t cap = 0;
    for (int i = 0; i < SCHEMA_SAMPLE_ROWS; i++) {
        ssize_t len = getline(&line, &cap, fp);
        if (len == -1)
            break; // Reached EOF before sample limit

        // strip return
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

    // prepare data buffers
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

        // strip return
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

                // reset buffers
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

        // flush
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

    // incomplete last chunk
    if (chunk_rows > 0) {
        table_insert_batch(db, (const void ***)col_data, num_cols, chunk_rows);
        for (size_t c = 0; c < num_cols; c++) {
            if (db->schema->columns[c].type != COL_TYPE_I32) {
                char **arr = (char **)col_data[c];
                for (size_t r = 0; r < chunk_rows; r++) free(arr[r]);
            }
        }
    }

    // cleanup
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
    if (rc != 0) {
        printf("Error: insert failed.\n");
    }

    free(i32_vals);
    free(str_vals);
    free(col_ptrs);
}

physical_operator_t *repl_exec_select(table_t *db, ast_query_t *ast) {

    char **column_names = calloc(sizeof(char *), ast->num_select_exprs);
    for (size_t i = 0; i < ast->num_select_exprs; i++) {
        column_names[i] = ast->select_exprs[i].column_name;
    }

    // selections
    physical_operator_t *op_scan = calloc(sizeof(physical_operator_t), 1);
    op_scan->type = OP_SCAN;
    op_scan->scan.scanner =
        table_scanner_create(db, (const char **)column_names, ast->num_select_exprs);

    physical_operator_t *root = op_scan;

    // filters
    if (ast->num_predicates > 0) {
        physical_operator_t *op_filter = calloc(sizeof(physical_operator_t), 1);

        op_filter->type = OP_FILTER;
        op_filter->num_children = 1;
        op_filter->children = calloc(1, sizeof(physical_operator_t *));
        op_filter->children[0] = op_scan;
        int column_id = -1;
        for (size_t i = 0; i < ast->num_select_exprs; i++) {
            if (strcmp(ast->predicates[0].column_name, column_names[i]) == 0) {
                column_id = i;
                break;
            }
        }

        if (column_id == -1) {
            printf("Error: column %s cannot be retrieved.\n", ast->predicates[0].column_name);
            return NULL;
        }

        op_filter->filter.column_idx = column_id;
        op_filter->filter.threshold = ast->predicates[0].int_val;
        switch (ast->predicates[0].op) {
        case OP_EQUALS:
            op_filter->filter.op = COMPARE_EQ;
            break;
        case OP_LESS_THAN:
            op_filter->filter.op = COMPARE_LT;
            break;
        case OP_GREATER_THAN:
            op_filter->filter.op = COMPARE_GT;
            break;
        }

        root = op_filter;
    }

    // aggregations
    if (ast->has_group_by) {
        physical_operator_t *op_agg = calloc(sizeof(physical_operator_t), 1);
        op_agg->type = OP_AGGREGATE;
        op_agg->num_children = 1;
        op_agg->children = calloc(1, sizeof(physical_operator_t *));
        op_agg->children[0] = root;
        op_agg->aggregate.aggregator = grace_hash_agg_create("/tmp", 1024, 4);

        int group_col_id = -1;
        int agg_col_id = -1;
        for (size_t i = 0; i < ast->num_select_exprs; i++) {
            if (strcmp(ast->group_by_col, column_names[i]) == 0) {
                group_col_id = (int)i;
            }
            if (ast->select_exprs[i].type == AST_AGG_SUM) {
                agg_col_id = (int)i;
            }
        }
        if (group_col_id == -1 || agg_col_id == -1) {
            printf("Error: invalid GROUP BY or SUM column.\n");
            return NULL;
        }
        op_agg->aggregate.group_by_col_idx = (size_t)group_col_id;
        op_agg->aggregate.agg_col_idx = (size_t)agg_col_id;
        root = op_agg;
    }

    return root;
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
            physical_operator_t *pipeline = repl_exec_select(db, &ast);
            if (pipeline) {
                while (true) {
                    block_t block = exec_pipeline_run(pipeline);
                    if (block.num_rows == 0)
                        break;
                    for (size_t r = 0; r < block.num_rows; r++) {
                        size_t idx = block.sel_vector ? block.sel_vector->data[r] : r;
                        for (size_t c = 0; c < block.num_columns; c++) {
                            printf("%s%d", c > 0 ? " | " : "", block.columns[c].data[idx]);
                        }
                        printf("\n");
                    }
                }
                physical_operator_destroy(pipeline);
            }
        }
    }

    free(line);
}
```

### `src/exec_pipeline.c`

```c
#include "simpledb/exec_pipeline.h"
#include "simpledb/filter.h"
#include "simpledb/grace_hash_agg.h"
#include "simpledb/table_scanner.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

block_t exec_pipeline_run(physical_operator_t *root) {
    switch (root->type) {
    case OP_SCAN:
        return table_scanner_next(root->scan.scanner);
    case OP_AGGREGATE: {
        if (!root->aggregate.aggregator->finished_ingestion) {
            while (1) {
                block_t block = exec_pipeline_run(root->children[0]);
                if (block.num_rows == 0)
                    break;

                agg_input_record_t *records = malloc(sizeof(agg_input_record_t) * block.num_rows);
                for (size_t i = 0; i < block.num_rows; i++) {
                    size_t physical_idx = block.sel_vector ? block.sel_vector->data[i] : i;
                    records[i].agg_value =
                        block.columns[root->aggregate.agg_col_idx].data[physical_idx];
                    records[i].group_key =
                        block.columns[root->aggregate.group_by_col_idx].data[physical_idx];
                }
                for (size_t k = 0; k < block.num_rows; k++) {
                    fflush(stdout);
                }
                grace_hash_agg_push(root->aggregate.aggregator, records, block.num_rows);
                free(records);
            }

            grace_hash_agg_finish(root->aggregate.aggregator);
        }

        block_t out_block = {0};
        out_block.num_columns = 2;

        out_block.columns = malloc(sizeof(slice_i32_t) * 2);
        int32_t *keys_buf = malloc(sizeof(int32_t) * 8192);
        int32_t *sums_buf = malloc(sizeof(int32_t) * 8192);
        out_block.columns[0] = slice_i32_create(keys_buf, 8192);
        out_block.columns[1] = slice_i32_create(sums_buf, 8192);

        agg_output_record_t out_rec;
        while (out_block.num_rows < 8192 &&
               grace_hash_agg_next(root->aggregate.aggregator, &out_rec)) {
            keys_buf[out_block.num_rows] = out_rec.group_key;
            sums_buf[out_block.num_rows] = (int32_t)out_rec.sum_value;
            out_block.num_rows++;
        }
        return out_block;
    }
    case OP_FILTER: {
        while (1) {
            block_t block = exec_pipeline_run(root->children[0]);
            if (block.num_rows == 0)
                return block;

            selection_vector_t *sel = selection_vector_create(block.num_rows);

            switch (root->filter.op) {
            case COMPARE_EQ:
                filter_eq_i32(block.columns[root->filter.column_idx], root->filter.threshold, sel);
                break;
            case COMPARE_GT:
                filter_gt_i32(block.columns[root->filter.column_idx], root->filter.threshold, sel);
                break;
            case COMPARE_LT:
                filter_lt_i32(block.columns[root->filter.column_idx], root->filter.threshold, sel);
                break;
            }

            if (sel->size == 0) {
                selection_vector_free(sel);
                free(block.columns);
                continue;
            }

            block.num_rows = sel->size;
            block.sel_vector = sel;
            return block;
        }
    }
    }
}

void physical_operator_destroy(physical_operator_t *op) {
    if (!op)
        return;
    for (size_t i = 0; i < op->num_children; i++) {
        physical_operator_destroy(op->children[i]);
    }
    if (op->children)
        free(op->children);
    free(op);
}
```

---

> **Day 22 Branch**: View the exact code for this day at [dmanchon/simpledb (`day22`)](https://github.com/dmanchon/simpledb/tree/day22) ([diff from `day21`](https://github.com/dmanchon/simpledb/compare/day21...day22)).

---

## 8. Post-Mortem: macOS vs. Linux CI Portability (`clangd` Auto-Includes & `_FORTIFY_SOURCE`)

When developing C11 on macOS with `clangd` (e.g., in Zed or VS Code) and pushing to an Ubuntu Linux CI runner (`gcc` with `-DCMAKE_BUILD_TYPE=Release`), two classic cross-platform traps surface:

### 1. Apple SDK Private Implementation Headers (`sys/_pthread/...`, `_string.h`, `_abort.h`)
On macOS, standard POSIX/C11 headers like `<pthread.h>`, `<string.h>`, `<stdlib.h>`, and `<sys/types.h>` internally split their declarations across private underscore-prefixed headers inside the Apple SDK. When `clangd` auto-imports a symbol such as `pthread_rwlock_t`, `ssize_t`, `memcpy`, or `abort()`, it sometimes inserts the physical sub-header where the symbol lives on Darwin rather than the public standard header:

* `#include <sys/_pthread/_pthread_rwlock_t.h>` in `src/table.c` (covered by standard `#include <pthread.h>`)
* `#include <_string.h>` in `src/external_sort.c` (covered by standard `#include <string.h>`)
* `#include <_abort.h>` in `src/table_scanner.c` (covered by standard `#include <stdlib.h>`)
* `#include <sys/_types/_ssize_t.h>` in `src/repl.c` (should be standard POSIX `#include <sys/types.h>`)

These underscore-prefixed headers do not exist in Linux `glibc`, causing an immediate `fatal error: No such file or directory` in CI. Always strip Darwin-internal underscore headers and rely strictly on the portable C11 / POSIX headers.

### 2. `glibc` `_FORTIFY_SOURCE` and `-Wunused-result` on `fread`
When CMake builds with `-DCMAKE_BUILD_TYPE=Release` (`-O3`) on Ubuntu GCC, `glibc` enables `_FORTIFY_SOURCE`, which annotates `fread()` with `__attribute__((warn_unused_result))`. Ignoring the return value of `fread()` triggers `-Wunused-result` warnings (and risks operating on uninitialized memory if an index file on disk is truncated). Wrapping `fread` in an explicit item-count check resolves the warning and hardens disk I/O:

```c
if (fread(&idx->header, sizeof(sparse_index_file_header_t), 1, fp) != 1) {
    fclose(fp);
    free(idx);
    return NULL;
}

if (fread(idx->marks, sizeof(int32_t), idx->header.num_marks, fp) != idx->header.num_marks) {
    fclose(fp);
    free(idx->marks);
    free(idx);
    return NULL;
}
```
