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

typedef enum {
    COMPARE_GT,
    COMPARE_EQ,
    COMPARE_LT
} compare_op_t;

// A simple filter that checks `column_idx` against a `threshold`
// (In a real DB, this would be a generic expression tree)
typedef struct {
    size_t column_idx;
    int32_t threshold;
    compare_op_t op;
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

