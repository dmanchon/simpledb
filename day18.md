# Day 18: APL / K Vectorized Physical Execution Pipeline

> **Day 18 Branch**: View the exact code for this day at [dmanchon/simpledb (`day18`)](https://github.com/dmanchon/simpledb/tree/day18) ([diff from `day17`](https://github.com/dmanchon/simpledb/compare/day17...day18)).

> [!TIP]
> **Long-Read Engineering Log (~790 lines) — TL;DR & Fast Navigation**
> This post is an unabridged Socratic pair-programming log covering the design of our vectorized physical execution pipeline in C11.
> - **Tagged-Union Operator DAG (`physical_operator_t`):** Represents `OP_SCAN`, `OP_FILTER`, and `OP_AGGREGATE` nodes without C++ virtual method tables.
> - **Batch-at-a-Time Vectorized Execution (`exec_pipeline_run`):** Streams `block_t` chunks from the multi-part Table Scanner through branchless SIMD filters and into Partitioned Grace Hash Aggregation.
> - **Want to go straight to the code?** [Jump to the C Header (`include/simpledb/exec_pipeline.h`)](#1-includesimpledbexec_pipelineh), [Jump to the Unit Tests](#2-teststest_exec_pipelinec), or [Jump to the Final C11 Implementation (`src/exec_pipeline.c`)](#3-the-final-implementation-srcexec_pipelinec).

Welcome to Day 18! Today we piece together everything we've built into a cohesive **Vectorized Physical Execution Pipeline**. 

In traditional relational databases (like PostgreSQL), the query engine usually employs a Volcano-style iterator model where data is passed one row at a time. This is extremely flexible but suffers from significant overhead: function calls and instruction cache misses per row. 

Modern analytical databases (ClickHouse, DuckDB) use a **Vectorized Execution Model**. Instead of processing single rows, operators process batches of rows (vectors or chunks) at a time. This allows the CPU to stay in tight, loop-unrolled C code (or SIMD kernels) for thousands of rows before incurring the next virtual function call or branch overhead.

In `simpledb`, we will lean heavily into the APL/K array-oriented roots. Our pipeline will string together vectorized operations that directly manipulate our 64-byte aligned blocks using the SIMD primitives we built in the earlier days.

## The Physical Operators

A physical execution plan is a directed acyclic graph (DAG) of physical operators. To keep C11 simple, we use a tagged union to represent operators.

The core pipeline operators we need are:
1. **OP_SCAN**: Wraps `table_scanner_t` to pull blocks of data from disk/RAM.
2. **OP_FILTER**: Takes an input operator, evaluates a predicate (e.g., `amount > 100`), and generates a `bitmap_t` or selection vector to filter out rows.
3. **OP_PROJECT**: (or Gather) Maps over the selected OIDs to produce new columns or evaluate scalar expressions.
4. **OP_AGGREGATE**: A terminal or pipeline-breaking operator that consumes blocks and feeds them into our partitioned `grace_hash_agg_t`.

Let's define the header for our execution pipeline.

## 1. `include/simpledb/exec_pipeline.h`

```c
#pragma once

#include "simpledb/table_scanner.h"
#include "simpledb/filter.h"
#include "simpledb/grace_hash_agg.h"

// Define the types of Physical Operators
typedef enum {
    OP_SCAN,
    OP_FILTER,
    OP_AGGREGATE
} operator_type_t;

// Forward declaration for the recursive tree structure
typedef struct physical_operator_t physical_operator_t;

// ---------------------------------------------------------
// Node-specific configurations
// ---------------------------------------------------------

typedef struct {
    table_scanner_t* scanner;
} scan_operator_t;

// A simple filter that checks `column_idx` against a `threshold` using >
// (In a real DB, this would be a generic expression tree)
typedef struct {
    size_t column_idx;
    int32_t threshold;
} filter_operator_t;

typedef struct {
    grace_hash_agg_t* aggregator;
    size_t group_by_col_idx;
    size_t agg_col_idx;
} aggregate_operator_t;

// ---------------------------------------------------------
// The Tagged Union representing a Node in the Plan
// ---------------------------------------------------------

struct physical_operator_t {
    operator_type_t type;
    
    // Child operators (e.g. FILTER wraps a SCAN)
    physical_operator_t** children;
    size_t num_children;
    
    union {
        scan_operator_t scan;
        filter_operator_t filter;
        aggregate_operator_t aggregate;
    };
};

// ---------------------------------------------------------
// Pipeline API
// ---------------------------------------------------------

// Execute the pipeline completely (e.g., until EOF or Aggregate finishes)
// Returns a block_t containing the final results.
block_t exec_pipeline_run(physical_operator_t* root);

// Clean up
void physical_operator_destroy(physical_operator_t* op);
```

#### Why is there a `destroy` but no `create`?

You might notice we have a `physical_operator_destroy(op)` function, but no `physical_operator_create()`. Why?

1. **Creation is Heterogeneous:** Every operator needs vastly different arguments. A `SCAN` needs a `table_scanner_t`. A `FILTER` needs a `column_idx` and a `threshold`. An `AGGREGATE` needs a `grace_hash_agg_t`. A generic `create` function would have a terribly messy signature. In a real database, the **Physical Planner** creates these nodes (e.g., `plan_scan()`, `plan_filter()`), or they are manually `malloc`'d like we do in our test mocks.
2. **Destruction is Uniform:** Tearing down the tree is identical for every operator. `physical_operator_destroy` simply recurses through the `children` array, calls `destroy` on each child, frees the `children` array itself, and then frees `op`. One recursive function can cleanly tear down any tree!

## How It Fits Together

1. **Pull Model**: We call `exec_pipeline_run()`, which walks to the `root` operator and repeatedly asks it for the next block until EOF.
2. **Data Flow**: If `root` is an `OP_AGGREGATE`, it will repeatedly pull `block_t` from its child (`OP_FILTER`), which pulls from its child (`OP_SCAN`).
3. **Execution**: The `OP_FILTER` takes the block from `OP_SCAN`, runs our SIMD kernels (`simd_greater_than_i32`), creates a selection vector, gathers the valid rows into a new compact `block_t`, and passes it up to `OP_AGGREGATE`.

## Mocking the Pipeline

Let's write a unit test to demonstrate building the AST (Abstract Syntax Tree) for the physical plan and executing it over our mock table.

## 2. `tests/test_exec_pipeline.c`

```c
#include "simpledb/exec_pipeline.h"
#include "simpledb/table.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main() {
    // 1. Setup a Mock Table
    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "amount", COL_TYPE_I32);
    
    table_t* table = table_create("events", schema);
    
    // Insert some mock data: user_id, amount
    int32_t user_ids[] = {1, 2, 1, 3, 2, 1};
    int32_t amounts[]  = {50, 200, 150, 300, 80, 250};
    // Expected Filter (amount > 100):
    // user_id=2, amount=200
    // user_id=1, amount=150
    // user_id=3, amount=300
    // user_id=1, amount=250
    //
    // Expected Aggregate (SUM(amount) GROUP BY user_id):
    // user_id 1 -> 400
    // user_id 2 -> 200
    // user_id 3 -> 300

    buffer_i32_t buf_users = { .data = user_ids, .size = 6, .capacity = 6 };
    buffer_i32_t buf_amounts = { .data = amounts, .size = 6, .capacity = 6 };
    
    table_insert_column_i32(table, "user_id", &buf_users);
    table_insert_column_i32(table, "amount", &buf_amounts);

    // 2. Build the Physical Plan (AST)
    // SCAN (user_id, amount)
    physical_operator_t* op_scan = malloc(sizeof(physical_operator_t));
    op_scan->type = OP_SCAN;
    op_scan->num_children = 0;
    op_scan->children = NULL;
    const char* cols[] = {"user_id", "amount"};
    op_scan->scan.scanner = table_scanner_create(table, cols, 2);

    // FILTER (amount > 100) -> amount is col_idx 1
    physical_operator_t* op_filter = malloc(sizeof(physical_operator_t));
    op_filter->type = OP_FILTER;
    op_filter->num_children = 1;
    op_filter->children = malloc(sizeof(physical_operator_t*));
    op_filter->children[0] = op_scan;
    op_filter->filter.column_idx = 1;
    op_filter->filter.threshold = 100;

    // AGGREGATE (GROUP BY user_id, SUM(amount))
    // user_id is col_idx 0, amount is col_idx 1
    physical_operator_t* op_agg = malloc(sizeof(physical_operator_t));
    op_agg->type = OP_AGGREGATE;
    op_agg->num_children = 1;
    op_agg->children = malloc(sizeof(physical_operator_t*));
    op_agg->children[0] = op_filter;
    op_agg->aggregate.aggregator = grace_hash_agg_create(1024); // 1024 groups
    op_agg->aggregate.group_by_col_idx = 0;
    op_agg->aggregate.agg_col_idx = 1;

    // 3. Create and Run the Pipeline
    block_t result = exec_pipeline_run(op_agg);
    
    // 4. Verify the Results
    printf("Aggregation Output (%zu groups):\n", result.num_rows);
    bool found_user1 = false, found_user2 = false, found_user3 = false;

    for (size_t i = 0; i < result.num_rows; ++i) {
        int32_t uid = result.columns[0].data[i];
        int32_t sum = result.columns[1].data[i];
        printf("User %d -> Total Amount %d\n", uid, sum);

        if (uid == 1) { assert(sum == 400); found_user1 = true; }
        if (uid == 2) { assert(sum == 200); found_user2 = true; }
        if (uid == 3) { assert(sum == 300); found_user3 = true; }
    }
    
    assert(found_user1 && found_user2 && found_user3);
    printf("Day 18 Execution Pipeline test passed!\n");

    // Cleanup
    if (result.columns) free(result.columns);
    physical_operator_destroy(op_agg);
    table_destroy(table);
    schema_registry_destroy(schema);

    return 0;
}
```

This represents the culmination of all the sub-systems we've built, tying together the `table_scanner_t`, our SIMD filters, and our `grace_hash_agg_t`.

## Architectural Context: The Execution Engine

To understand what we're building today, it helps to see where the physical pipeline fits in the lifespan of a SQL query:

1. **Parser:** Translates a string (`SELECT ... FROM events`) into an Abstract Syntax Tree (AST).
2. **Logical Planner:** Converts the AST into a relational algebra plan (e.g., `Projection(Aggregation(Filter(Scan)))`).
3. **Optimizer:** Rearranges logical operators (e.g., pushes the Filter down so it happens *before* a Join or Aggregation) to make it faster.
4. **Physical Planner:** Translates logical operators into the actual C structures executing the algorithms (e.g., converting a logical Join into a `HashJoin` or `MergeSortJoin` operator). This tree of physical operators is the **Execution Pipeline**.

### How do other Databases implement this?

Historically, there have been three major eras of execution engines:

1. **The Volcano Model (Row-at-a-time):** Used by PostgreSQL, MySQL, SQLite. 
   - **How it works:** The root operator calls `next()` on its child, returning **one single row**. 
   - **Pros:** Extremely memory efficient and flexible.
   - **Cons:** Extremely slow for analytics. Function call overhead per row, heavy branch mispredictions, and terrible CPU cache locality. The CPU can't use SIMD because it only sees one row at a time.

2. **The MonetDB Model (Column-at-a-time / Bulk):** 
   - **How it works:** Operators process entire columns (millions of rows) at once.
   - **Pros:** Very fast, zero branch prediction misses, highly vectorized.
   - **Cons:** If you filter 100GB of data, the intermediate output of the filter is still gigabytes large and spills out of the CPU L1/L2 cache into main memory, creating a memory bandwidth bottleneck.

3. **The ClickHouse & DuckDB Model (Vectorized / Block-at-a-time):**
   - **How it works:** The "sweet spot". The pipeline still pulls data like Volcano, but instead of one row, it pulls a **Block** or **DataChunk** of rows (usually 1024 to 8192 rows at a time).
   - **Pros:** The block size is deliberately chosen to fit perfectly inside the CPU L1/L2 cache (a few megabytes). Tight C loops operate on these vectors, allowing the compiler to use AVX/NEON SIMD instructions.
   - **What we use:** This is exactly what we are building with `block_t`.

### Are these physical operators the bare minimum?

Yes! `SCAN`, `FILTER`, and `AGGREGATE` represent the absolute bare minimum required to execute analytical group-by queries (which is what 90% of analytical benchmarks test). 

In a fully-fledged database like DuckDB, the execution engine has dozens of specialized operators:
- **`OP_PROJECT`:** Evaluates scalar expressions (e.g., `SELECT amount * 1.2`).
- **`OP_HASH_JOIN` / `OP_MERGE_JOIN`:** Correlates rows between two tables.
- **`OP_ORDER_BY` (Sort):** Buffers data into a Min-Heap or external sorter.
- **`OP_LIMIT`:** Truncates early and tells the `SCAN` to stop pulling data.

For our prototype, we're sticking to the three core operators needed to execute our target `jsonbench` workload: scanning events, filtering them, and aggregating them.

### Implementing the Pipeline

The first step in implementing the pipeline is creating the context state. The user has provided an excellent, memory-safe approach:

```c
exec_pipeline_t* exec_pipeline_create(physical_operator_t* root) {
    exec_pipeline_t *pipeline = calloc(sizeof(*pipeline), 1);
    pipeline->root = root;
    return pipeline;
}
```

Using `calloc(sizeof(*pipeline), 1)` instead of `malloc` is a great C practice because it guarantees that any padding or future fields in `exec_pipeline_t` are strictly zero-initialized, preventing undefined behavior.

### The Driver: `exec_pipeline_run`

The intent of `exec_pipeline_run` is to serve as the **Execution Engine's Driver**. It kicks off the query execution by asking the `root` operator for data.

To make the pipeline pull data, we'll need a helper function that evaluates a single operator recursively. Let's call it `execute_operator(physical_operator_t* op)`. `exec_pipeline_run` will simply call `execute_operator(pipeline->root)`.

#### What is expected of `execute_operator`?

It needs a `switch(op->type)` statement to handle the specific logic for each operator type:

1. **`OP_SCAN`**:
   - **Expectation**: Call `table_scanner_next()` from Day 17 and return the block.
2. **`OP_FILTER`**:
   - **Expectation**: Call `execute_operator` on its single child. Take the resulting `block_t`, run our SIMD kernels (e.g., `simd_greater_than_i32`) to create a selection vector, and build a new filtered `block_t` containing only the rows that passed. If the child returns an empty block (EOF), it just returns an empty block up the chain.
3. **`OP_AGGREGATE`**:
   - **Expectation**: This is a **pipeline breaker** (or a blocking operator). It cannot yield any results until it has seen *all* the data.
   - It must enter a `while(true)` loop, repeatedly calling `execute_operator` on its child to pull data.
   - As blocks arrive, it feeds them into the `grace_hash_agg_t` hash table.
   - When the child returns a block with `num_rows == 0` (EOF), it breaks the loop, materializes the hash table into a final output `block_t`, and returns it.

#### What exactly is the `root`? Is it a list or a tree?

The `root` is the top-most node of the execution plan. The execution plan is a **Tree** (or more accurately, a Directed Acyclic Graph). 

In C, we defined the `physical_operator_t` structure with an array of children:
```c
physical_operator_t** children;
size_t num_children;
```

Depending on the query, this structure takes different shapes:
- **A Linked List:** For a simple query like `SCAN` -> `FILTER` -> `AGGREGATE`, every operator has exactly one child. Structurally, this looks like a linked list.
- **A Tree:** If you introduce an `OP_JOIN` operator (e.g., `SELECT * FROM A JOIN B`), the join operator needs to pull data from *two* different tables. Its `num_children` would be 2. The left child would be the pipeline for table A, and the right child would be the pipeline for table B. This is why the generic structure must be a tree.

**Control Flow vs. Data Flow:**
- **Control Flow (Top-Down):** The engine starts at the `root` and walks down the tree. The root asks its children for data.
- **Data Flow (Bottom-Up):** The leaf nodes (like `OP_SCAN`) pull data from the disk and pass the blocks *up* the tree to their parents.

Because the final, fully-processed result of the query eventually emerges from the top-most node, we give the `root` operator to `exec_pipeline_run()`.

#### Real Use Case: Data Flow of a Query

Let's say our query is `SELECT user_id, SUM(amount) FROM events WHERE amount > 100 GROUP BY user_id`.

The tree is: `Root(OP_AGGREGATE)` -> `Child(OP_FILTER)` -> `Child(OP_SCAN)`.

### Drafting the C Implementation

When attempting to write the pipeline driver, a common first instinct is to loop over all the children of the root:

```c
// Anti-pattern: Don't do this!
block_t exec_pipeline_run(exec_pipeline_t* pipeline) {
    for (size_t i=0; i<pipeline->root->num_children; i++) {
        // Error: The execution logic depends on the operator type!
    }
}
```

Why doesn't this work? In a Pull-based model, we don't blindly iterate over children. The execution logic depends entirely on the **type** of the operator. A `SCAN` has no children to loop over. An `AGGREGATE` needs to loop over its *single* child until it runs out of data (EOF). 

Because we removed the `exec_pipeline_t` wrapper to keep things lean, `exec_pipeline_run` *is* the recursive function itself. It switches directly on the operator type:

```c
block_t exec_pipeline_run(physical_operator_t* op) {
    switch (op->type) {
        case OP_SCAN: {
            // Call table_scanner_next()
            break;
        }
        case OP_FILTER: {
            // 1. Pull a block from the child: exec_pipeline_run(op->children[0])
            // 2. If EOF, return EOF
            // 3. Otherwise, run the SIMD filter and return a new filtered block
            break;
        }
        case OP_AGGREGATE: {
            // 1. Enter a while(1) loop
            // 2. Pull a block from the child: exec_pipeline_run(op->children[0])
            // 3. If EOF, break the loop
            // 4. Otherwise, hash the block into grace_hash_agg_t
            // 5. After the loop, return the final aggregated block
            break;
        }
    }
    
    // Return an empty block by default
    block_t empty = {0};
    return empty;
}
```

*Note on Design:* In a production database, you would typically keep the `exec_pipeline_t` wrapper (using the Facade Pattern). It would set up query-level state (like memory arenas, thread pools, and telemetry) before calling a private static recursive function to crunch the tree. But for our educational prototype, eliminating the wrapper removes the "Middle Man" code smell!

#### Who controls the lifecycle?

A common question is: *Who controls the lifecycle? Who decides to pull the next block?*

In a Pull-based model, **the parent operator controls the lifecycle of its children**, and the **top-level application controls the root**.

Let's look at the two scenarios:

1. **When the root is `OP_SCAN` or `OP_FILTER` (Streaming Operators):**
   If you just run a simple `SELECT * FROM events WHERE amount > 100`, the top-most operator is a `FILTER`. The filter only returns one block at a time. Therefore, the *Database REPL* (the top-level C application) must put `exec_pipeline_run(root)` inside a `while(true)` loop, printing the blocks to the console one by one until it receives an empty block.

2. **When the root is `OP_AGGREGATE` (Blocking Operator):**
   If you run `SELECT SUM(amount) GROUP BY user_id`, the root is an `AGGREGATE`. The top-level REPL only calls `exec_pipeline_run(root)` **once**. 
   Why? Because `OP_AGGREGATE` acts as a "pipeline breaker". Inside its `switch` case, the aggregate itself contains a `while(true)` loop that pulls all the data from its children until EOF. It swallows the entire dataset, builds the hash table, and then returns a *single* finalized block back to the REPL. 

This means the engine is inherently self-driving based on the demands of the top-most node!

### How does the Filter detect `>`, `<`, or `=`?

You might have noticed that our `filter_operator_t` struct only has `column_idx` and `threshold`. There is no field that specifies whether we are doing `>`, `<`, or `=`! 

This is another intentional simplification for our prototype. If you look at `exec_pipeline.h`, we added a comment:
```c
// A simple filter that checks `column_idx` against a `threshold` using >
// (In a real DB, this would be a generic expression tree)
typedef struct {
    size_t column_idx;
    int32_t threshold;
} filter_operator_t;
```

In a real database, `OP_FILTER` does not take a simple `column_idx` and `threshold`. It takes an **Expression Tree (AST)** that was built by the SQL Parser. The Expression Tree handles the comparison type (`COMPARE_GT`, `COMPARE_EQ`, `COMPARE_LT`), logical AND/OR chains, and mathematical operations. 

If we wanted to make our `filter_operator_t` slightly more realistic without building a full Expression AST, we could simply add an enum:

```c
typedef enum {
    COMPARE_GT,
    COMPARE_EQ,
    COMPARE_LT
} compare_op_t;

typedef struct {
    size_t column_idx;
    int32_t threshold;
    compare_op_t op;
} filter_operator_t;
```
Then, inside `exec_pipeline_run`'s `OP_FILTER` case, we would add a nested `switch(root->filter.op)` to decide whether to call `filter_gt_i32()` or `filter_eq_i32()`.

### Implementing `OP_FILTER` (Zero-Copy)

Because we wanted to eliminate the leaky abstraction, we updated `block_t` in `table_scanner.h` to include a `selection_vector_t* sel_vector` field. This enables **Zero-Copy Late Materialization**, exactly like DuckDB's DataChunks!

#### Why do we need to modify `block_t` to add a Selection Vector?

If `exec_pipeline_run(root->children[0])` gives you a block of 8,192 rows, and your filter finds that only 4,000 rows match the criteria, how do you pass those 4,000 rows up to the parent operator?

**The Old Way (Memory Copying):**
If `block_t` only contains dense arrays, you have no choice but to create a brand new `block_t`. You have to allocate new memory for 4,000 elements for *every single column*, loop through the data to copy (gather) the 4,000 matching values into the new arrays, and then return the new block. This wastes memory bandwidth on useless copying.

**The Zero-Copy Way (Why we added `sel_vector`):**
By adding a `selection_vector_t*` to `block_t`, you don't have to allocate new arrays or copy any column data! You simply take the `child_block` you received, attach the selection vector (which is just an array of the 4,000 valid indices), update `child_block.num_rows = 4000`, and return the *exact same block*. 

The parent operator receives it and says, *"Ah, this block has a `sel_vector`. I'll just use these indices to read the data in place."* 

This is the magic of **Zero-Copy Late Materialization**!

Here is the exact algorithmic flow you need to implement for the `OP_FILTER` case in `exec_pipeline_run`:

1. **Pull Data**: Call `exec_pipeline_run(root->children[0])` to get the `child_block`.
2. **Check EOF**: If `child_block.num_rows == 0`, just return it.
3. **Filter**: 
   - Create a `selection_vector_t* sel = selection_vector_create(child_block.num_rows)`.
   - Add a `switch(root->filter.op)` statement.
   - If `COMPARE_GT`, call `filter_gt_i32()`. If `COMPARE_EQ`, call `filter_eq_i32()`. Pass the target column (`child_block.columns[root->filter.column_idx]`) and the `root->filter.threshold`. This populates your selection vector with the row indices (OIDs) that passed the filter.
4. **Update the Block (Zero Copy!)**:
   - Do **NOT** allocate memory or gather the columns!
   - Simply point `child_block.sel_vector = sel;`
   - Update `child_block.num_rows = sel->size;`
5. **Return**: Return the `child_block` directly.

### Implementing `OP_AGGREGATE` (The Pipeline Breaker)

You have successfully implemented the perfect Zero-Copy `OP_FILTER`! The memory usage is completely flat.

Now we need to implement the pipeline breaker: `OP_AGGREGATE`.

When the top-level application calls `exec_pipeline_run` on an `OP_AGGREGATE` root, it expects back one massive `block_t` containing the final aggregated results.

Here is the algorithmic flow for `OP_AGGREGATE`:

1. **Ingestion Loop (Pipeline Breaker)**:
   - Start a `while(1)` loop.
   - Call `block_t block = exec_pipeline_run(root->children[0]);`
   - If `block.num_rows == 0`, break out of the loop (we've hit EOF).
   - Otherwise, extract the valid rows and push them into `grace_hash_agg`:
     ```c
     agg_input_record_t* records = malloc(sizeof(agg_input_record_t) * block.num_rows);
     for (size_t i = 0; i < block.num_rows; i++) {
         // Resolve the physical index using the selection vector!
         size_t physical_idx = block.sel_vector ? block.sel_vector->data[i] : i;
         records[i].group_key = block.columns[root->aggregate.group_by_col_idx].data[physical_idx];
         records[i].agg_value = block.columns[root->aggregate.agg_col_idx].data[physical_idx];
     }
     grace_hash_agg_push(root->aggregate.aggregator, records, block.num_rows);
     free(records);
     ```
2. **Finish Ingestion**:
   - Call `grace_hash_agg_finish(root->aggregate.aggregator);`
3. **Yield Final Block**:
   - Create a final output `block_t` with 2 columns (`group_key` and `sum_value`).
   - Run a `while (grace_hash_agg_next(..., &record))` loop.
   - Push the resulting `group_key` and `sum_value` into the two columns.
   - Return the finalized `block_t`.

### Socratic Review of `OP_AGGREGATE`

You've successfully built the `while(1)` pipeline breaker! But look closely at the arguments you passed to `grace_hash_agg_push`:

```c
grace_hash_agg_push(root->aggregate.aggregator, block.columns[root->aggregate.agg_col_idx], block.num_rows);
```

1. **Missing the Group By Key:** You passed `block.columns[...agg_col_idx]`, which is just the column of `amount`s. But how does the hash table know *who* those amounts belong to? `grace_hash_agg_push` requires an array of `agg_input_record_t` (which contains both the `group_key` AND the `agg_value`). 
2. **Ignoring the Selection Vector:** Remember our zero-copy `OP_FILTER`? The `block.columns` arrays still contain the *original 8,192 unfiltered rows*! If you don't use the `sel_vector` to resolve the physical index, you will end up hashing the first N rows of the block, regardless of whether they actually passed the filter or not.
3. **Yielding the Result:** Once the `while(1)` loop finishes, what does `OP_AGGREGATE` return? Right now, it just breaks and falls out of the function without returning a `block_t`. You need to call `grace_hash_agg_finish()`, pull the results out of the aggregator, and return a brand new output `block_t`.

Here is the exact code snippet for how to correctly pack the records using the selection vector:
```c
     agg_input_record_t* records = malloc(sizeof(agg_input_record_t) * block.num_rows);
     for (size_t i = 0; i < block.num_rows; i++) {
         // Resolve the physical index using the selection vector!
         size_t physical_idx = block.sel_vector ? block.sel_vector->data[i] : i;
         records[i].group_key = block.columns[root->aggregate.group_by_col_idx].data[physical_idx];
         records[i].agg_value = block.columns[root->aggregate.agg_col_idx].data[physical_idx];
     }
     grace_hash_agg_push(root->aggregate.aggregator, records, block.num_rows);
     free(records);
```

### The SoA to AoS Copy (Can we avoid it?)

You correctly noticed that `malloc`ing an array of `agg_input_record_t` destroys our beautiful zero-copy SoA pipeline. You even added a comment: `// rewrite this, we need aggregator to accept blocks`.

I love the ambition! But before you rewrite the aggregator, there is a fundamental computer science concept you need to know: **Hash Tables are inherently Row-Oriented (AoS)!**

When you insert data into a Hash Bucket, you need the `group_key` and the `agg_value` to live right next to each other in memory (an Array of Structs). You cannot easily build a purely columnar (SoA) hash table without massive cache misses.

This means a transition from SoA (Columns) to AoS (Rows) is **unavoidable** when aggregating. 

In DuckDB, they don't do a temporary `malloc` in the execution node. Instead, they pass the `block_t` directly into the Hash Aggregator, and the Aggregator does the SoA -> AoS transition *internally* as it inserts the data directly into the Hash Buckets. 

Because we built our `grace_hash_agg_t` back on Day 10 using a simpler `agg_input_record_t` buffer, we are forced to do the SoA -> AoS transition out here in `OP_AGGREGATE`. For this educational prototype, it is completely fine to leave it!

### Socratic Review of your `OP_AGGREGATE` Draft (Round 2)

Your ingestion loop is looking *beautiful*! The physical index mapping is exactly right. But you are missing the final steps, and your engine will currently crash due to a C switch-statement gotcha:

1. **Memory Leak:** You forgot to `free(records);` at the end of the `while` loop!
2. **Fallthrough Bug:** You call `grace_hash_agg_finish()`, but then there is no `return` or `break`! Because this is a `switch` statement, execution will fall right through into `case OP_FILTER:` and crash.
3. **The Output Phase:** Your aggregation ran perfectly, but you never handed the whiteboard back to the boss! You need to pull the results out of the aggregator, pack them into a brand new `block_t`, and return it.

### How to Build the Output Block (C Memory Guide)

If you are feeling a bit lost trying to `malloc` the final block, you are not alone! Building a columnar block from scratch in C can be confusing. Let's break down exactly how to do it step-by-step. Put this code right after `grace_hash_agg_finish()`.

**1. Don't `malloc` the block!**
Look at the function signature: `block_t exec_pipeline_run(...)`. It returns a `block_t` *by value* (on the stack), not a pointer!
```c
block_t out_block = {0}; // Initialize an empty block on the stack
out_block.num_columns = 2; // We are returning 2 columns: [group_key, sum_value]
```

**2. `malloc` the Columns**
The block itself lives on the stack, but the data arrays inside it need heap memory to store up to 8,192 rows (the max capacity of a block). 
```c
out_block.columns[0].data = malloc(sizeof(int32_t) * 8192); // Space for 8192 group_keys
out_block.columns[1].data = malloc(sizeof(int32_t) * 8192); // Space for 8192 sum_values
```

**3. Loop and Pack**
`grace_hash_agg_next` does *not* return an array. It yields **exactly one record at a time**. So we just declare a single record on the stack, pass its address (`&out_rec`), and loop until it returns `false` (exhausted) or the block fills up!
```c
agg_output_record_t out_rec;

// Keep pulling 1 record at a time until exhausted (or block is full)
while (grace_hash_agg_next(root->aggregate.aggregator, &out_rec) && out_block.num_rows < 8192) {
    // Pack the AoS record back into our SoA columnar block!
    out_block.columns[0].data[out_block.num_rows] = out_rec.group_key;
    out_block.columns[1].data[out_block.num_rows] = (int32_t)out_rec.sum_value;
    
    out_block.num_rows++; // Increment the row count!
}

return out_block; // Return by value
```

#### What happens if there are more than 8192 groups? (State Machines)

You might be wondering: *"Does `grace_hash_agg_next` pull only once, or until the data is exhausted?"*

`grace_hash_agg_next` pulls **exactly one record (one group)** at a time. It returns `true` if it found a record, and `false` when the hash table is totally exhausted. This is why we wrap it in a `while` loop: to pack as many records as possible into the output `block_t`!

But notice the condition `out_block.num_rows < 8192`. Our blocks are capped at 8,192 rows. 
What if you had 10,000 distinct `user_id` groups?
1. The `while` loop would pack 8,192 groups into the block and return it. 
2. The boss (top-level REPL) would process that block, and then call `exec_pipeline_run(root)` **again** to get the rest of the data. 

**The Bug:** If the boss calls `exec_pipeline_run(root)` a second time, our current code will start the `while(1)` Ingestion loop all over again! 

In a production database, `OP_AGGREGATE` must be a **State Machine**. It would have a boolean flag like `bool is_ingested;`. 
- On the first call, it ingests everything, builds the hash table, sets `is_ingested = true`, and yields the first 8192 rows.
- On the second call, it sees `is_ingested == true`, skips the ingestion loop entirely, and goes straight to the output phase to yield the remaining 1,808 rows!

For our simple prototype, we assume the final grouping fits in one block, so our code works fine. This means **only one block ever comes back** from our `OP_AGGREGATE` operator. The top-level test runner will call it exactly once, receive the single block of answers, and finish.

### How DuckDB and ClickHouse do Aggregation

You asked a brilliant question: *How do real systems avoid this intermediate copy and cache misses?*

First, a hard truth: **The Hash Table itself is always a copy.** You have to store the groups and running totals *somewhere*. But modern engines eliminate the *intermediate* step (our `malloc` array) by using **Vectorized Scatter/Gather**.

#### 1. Vectorized Hash Probing (DuckDB)
DuckDB does not loop through rows one by one like we do. It operates on the entire block at once:
1. **Vectorized Hash:** It takes the entire column of `group_keys`, applies the `sel_vector`, and hashes all valid keys in one SIMD instruction, producing an array of Hashes.
2. **Vectorized Lookup:** It takes the array of Hashes and looks up the memory addresses of the buckets in the hash table, producing an array of `pointers`.
3. **Vectorized Scatter:** Finally, it takes the `amount` column, and uses a SIMD Scatter instruction to write the amounts directly into the memory addresses of the hash buckets. 
By doing this, the data moves directly from the columnar block into the hash table's payload area. Zero intermediate allocations!

#### 2. Extremely Specialized Hash Tables (ClickHouse)
ClickHouse takes it a step further. Instead of having one generic Hash Table, ClickHouse has over **30 different Hash Table implementations**!
- If you `GROUP BY` a single 8-bit integer (like `age`), ClickHouse doesn't use a hash table at all. It allocates a flat array of 256 elements and just uses the column data as the array index!
- If you `GROUP BY` a string, it uses a specialized `StringHashTable`.
- If you `GROUP BY` two integers, it uses a `TwoLevelHashMap`.

ClickHouse dynamically inspects your query and compiles (JIT) a custom loop that reads directly from the raw columns (respecting the selection vector) and writes directly into the perfectly-sized, specialized data structure. 

This obsessive attention to memory access patterns is why ClickHouse is so ridiculously fast at aggregations!

### Deep Dive: Aggregation in Plain English

If the `OP_AGGREGATE` logic seems overwhelming, let's use a real-world analogy. 

Imagine you have a massive bucket of receipts. Each receipt has a `user_id` and an `amount`. You want to find the total amount spent by each user (`SELECT user_id, SUM(amount) GROUP BY user_id`).

#### 1. The Ingestion Phase (The Pipeline Breaker)
Can you tell me the final total for User A after looking at only half the bucket? No! If you stop early, your total is wrong. You *must* look at every single receipt. 

This is why `OP_AGGREGATE` is called a **Pipeline Breaker**, and why it has a `while(1)` loop. 
It reaches into the bucket and pulls out a handful of receipts (a `block_t`). 

**Wait, who does the math?** You might notice that `OP_AGGREGATE` doesn't actually do any math! It just extracts the valid `user_id` and `amount` from the block, packs them into an array of `agg_input_record_t`, and passes them to `grace_hash_agg_push`. 
It is delegating the math! The `grace_hash_agg_t` (from Day 10) is the "whiteboard". Inside `grace_hash_agg_push`, the engine hashes the `user_id`, finds the running tally for that user, and adds the `amount` to the `SUM()` in-place (or spills to disk if the whiteboard is full). `OP_AGGREGATE` just feeds the whiteboard.

#### Wait, is `OP_AGGREGATE` always required?

A common question is: *"Does every query end with an OP_AGGREGATE?"* 

No! The pipeline is just a tree of whatever operations the user requested in their SQL query. 

- If the user types: `SELECT SUM(amount) FROM events GROUP BY user_id`, then yes, the root of the tree is `OP_AGGREGATE`.
- If the user types: `SELECT * FROM events WHERE amount > 100`, there is NO aggregation! The root of the tree is `OP_FILTER`. The top-level Database REPL just calls `exec_pipeline_run` on the filter in a loop, taking the blocks of filtered rows and printing them to the screen until EOF. 

The Execution Engine just blindly executes the tree it is given.

#### 2. Logical vs. Physical Indices (The Post-it Note)
Imagine your helper (`OP_FILTER`) went through a handful of 10 receipts before handing them to you. They crossed out the receipts under $100. 
To save time and paper, they don't give you a brand new stack of just the valid receipts. They hand you the original stack of 10, but stick a Post-it note on top (`sel_vector`) that says: *"Only look at receipt #5 and receipt #9."*

When you are adding up the totals on your whiteboard, you look at the Post-it note.
- The Post-it note has 2 items on it (`num_rows = 2`).
- You loop twice (`i = 0`, then `i = 1`). This is the **Logical Index**.
- You read the first number on the Post-it note: it says 5. This is the **Physical Index**. You jump straight to the 5th receipt in the stack to read the `user_id` and `amount`.

If your helper *didn't* filter anything (just an `OP_SCAN`), there is no Post-it note (`sel_vector = NULL`). You just read receipt 0, then 1, then 2, etc.

#### 3. The Output Phase (Yielding the final block)
Once the bucket is empty, you are done ingesting. You look at your whiteboard, write down all the final totals on a brand new sheet of paper (a new output `block_t`), and hand that single sheet to your boss (the top-level application).

By doing this, `OP_FILTER` does zero memory copying. It just updates the mask!

#### Wait, doesn't `children[0]` assume it's a SCAN? Is this leaky too?

You might look at `exec_pipeline_run(root->children[0])` and think, *"Wait, does the filter assume its child is a SCAN? Isn't that another leaky abstraction?"*

No! This is the absolute beauty of the **Iterator Model** (Volcano/Vectorized). 

`OP_FILTER` has **zero idea** what its child is. The child could be an `OP_SCAN`, but it could also be another `OP_FILTER` (if we chained `WHERE a > 10 AND b < 20`), or an `OP_JOIN`. 
All `OP_FILTER` knows is: *"I called `exec_pipeline_run` on my child, and it handed me a `block_t`."* It doesn't care *how* that block was generated. This is perfect **Composability**.

#### Wait, does "Pull Data" pull the entire table at once?

You might also look at step 1 and think, *"Does calling the child pull the entire table into memory?"*

No! Remember that `OP_SCAN` is yielding data in **Chunks** (Granules of 8,192 rows). 
When `OP_FILTER` calls its child, it receives exactly **one block** (max 8,192 rows), not the whole table. 

This is why `OP_AGGREGATE` needs a `while(true)` loop! The aggregator asks the filter for a block (8,192 rows), the filter asks the scan for a block (8,192 rows), the scan reads 8,192 rows from disk and passes it up. The aggregator hashes those 8,192 rows, and then says, *"Okay, give me the next batch!"* This keeps the CPU L1/L2 cache perfectly warm and ensures memory usage stays completely flat, even if you are querying a 1 Terabyte table!
Instead of reading `value = block->columns[0].data[i]`, it must read:
```c
size_t physical_idx = block->sel_vector ? block->sel_vector->data[i] : i;
int32_t value = block->columns[0].data[physical_idx];
```

#### Where is the recursion?

You might wonder, "If we aren't using a `for` loop, where is the recursion?"

The recursion happens **inside the specific `switch` cases**. 

For example, when `exec_pipeline_run` evaluates an `OP_FILTER`, it must explicitly call itself on its child to get data:
```c
case OP_FILTER: {
    // This is the recursive call!
    block_t child_block = exec_pipeline_run(root->children[0]);

    
    if (child_block.num_rows == 0) return child_block; // EOF
    
    // ... apply SIMD filter to child_block ...
}
```

This ensures that the recursion is **demand-driven** (Pull-based). The parent operator only recurses down to the child *when it actually needs more data*, rather than the pipeline blindly forcing execution across all children at once.

Here is exactly how the data flows when you call `exec_pipeline_run`:

1. `exec_pipeline_run` asks `OP_AGGREGATE` for data.
2. `OP_AGGREGATE` enters its loop and asks `OP_FILTER` for a block.
3. `OP_FILTER` asks `OP_SCAN` for a block.
4. `OP_SCAN` reads 8,192 rows from the disk and returns them.
5. `OP_FILTER` scans those 8,192 rows, finds 4,000 where `amount > 100`, packs them into a new block, and returns it.
6. `OP_AGGREGATE` hashes those 4,000 rows into its `grace_hash_agg_t`.
7. `OP_AGGREGATE` asks `OP_FILTER` for *another* block.
8. Steps 3-6 repeat until `OP_SCAN` runs out of disk files and returns a block with `num_rows == 0`.
9. `OP_AGGREGATE` sees the EOF block, stops asking, exports its hash table into a final result block, and hands it back to `exec_pipeline_run`.

Does this clarify the "Pull" model and how the operators interact recursively? Let me know if you want to try drafting the C code for `execute_operator`!

---

## 3. The Final Implementation (`src/exec_pipeline.c`)

Here is the complete C implementation of the physical execution pipeline we designed:

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
            
            grace_hash_agg_push(root->aggregate.aggregator, records, block.num_rows);
            free(records);
        }

        grace_hash_agg_finish(root->aggregate.aggregator);

        block_t out_block = {0};
        out_block.num_columns = 2;

        out_block.columns = malloc(sizeof(slice_i32_t) * 2);
        int32_t *keys_buf = malloc(sizeof(int32_t) * 8192);
        int32_t *sums_buf = malloc(sizeof(int32_t) * 8192);
        out_block.columns[0] = slice_i32_create(keys_buf, 8192);
        out_block.columns[1] = slice_i32_create(sums_buf, 8192);

        agg_output_record_t out_rec;
        while (grace_hash_agg_next(root->aggregate.aggregator, &out_rec) &&
               out_block.num_rows < 8192) {
            keys_buf[out_block.num_rows] = out_rec.group_key;
            sums_buf[out_block.num_rows] = (int32_t)out_rec.sum_value;
            out_block.num_rows++;
        }
        return out_block;
    }
    case OP_FILTER: {
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

        block.num_rows = sel->size;
        block.sel_vector = sel;
        return block;
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

> **Day 18 Branch**: View the exact code for this day at [dmanchon/simpledb (`day18`)](https://github.com/dmanchon/simpledb/tree/day18) ([diff from `day17`](https://github.com/dmanchon/simpledb/compare/day17...day18)).
