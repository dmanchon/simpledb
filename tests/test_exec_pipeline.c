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
    
    table_t* table = table_create("events");
    table->schema = schema;
    
    buffer_i32_t* buf0 = malloc(sizeof(buffer_i32_t));
    buf0->data = malloc(sizeof(int32_t) * 6);
    buf0->size = 6;
    buf0->capacity = 6;

    buffer_i32_t* buf1 = malloc(sizeof(buffer_i32_t));
    buf1->data = malloc(sizeof(int32_t) * 6);
    buf1->size = 6;
    buf1->capacity = 6;

    table->column_buffers = malloc(sizeof(void*) * 2);
    table->column_buffers[0] = buf0;
    table->column_buffers[1] = buf1;
    table->row_count = 6;

    // Insert some mock data: user_id, amount
    int32_t user_ids[] = {1, 2, 1, 3, 2, 1};
    int32_t amounts[]  = {50, 200, 150, 300, 80, 250};
    
    for (int i = 0; i < 6; i++) {
        ((int32_t*)buf0->data)[i] = user_ids[i];
        ((int32_t*)buf1->data)[i] = amounts[i];
    }

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
    op_filter->children = calloc(1, sizeof(physical_operator_t*));
    op_filter->children[0] = op_scan;
    op_filter->filter.column_idx = 1;
    op_filter->filter.threshold = 100;
    op_filter->filter.op = COMPARE_GT;

    // AGGREGATE (GROUP BY user_id, SUM(amount))
    // user_id is col_idx 0, amount is col_idx 1
    physical_operator_t* op_agg = malloc(sizeof(physical_operator_t));
    op_agg->type = OP_AGGREGATE;
    op_agg->num_children = 1;
    op_agg->children = malloc(sizeof(physical_operator_t*));
    op_agg->children[0] = op_filter;
    op_agg->aggregate.aggregator = grace_hash_agg_create("/tmp", 1024, 2); // 1024 groups, 2 partitions
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
    

    return 0;
}
