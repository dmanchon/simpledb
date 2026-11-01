#include "simpledb/allocator.h"
#include "simpledb/benchmark.h"
#include "simpledb/mmap_file.h"
#include "simpledb/table.h"
#include "simpledb/table_scanner.h"
#include "simpledb/sql_parser.h"
#include "simpledb/repl.h"
#include "simpledb/exec_pipeline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_ROWS 10000000ULL

static double run_repl_sql_pass(table_t *table, const char *sql_query) {
    uint64_t t0 = benchmark_now_ns();

    // 1. Full SQL Lexer & Parser
    sql_parser_t parser = sql_parser_create(sql_query);
    ast_query_t ast = {0};
    bool ok = sql_parser_parse(&parser, &ast);
    assert(ok);
    (void)ok;

    // 2. Query Planner & Physical Operator Tree Binder
    physical_operator_t *pipeline = repl_exec_select(table, &ast);
    assert(pipeline != NULL);

    // 3. Vectorized Execution Engine Run
    size_t groups_count = 0;
    while (true) {
        block_t block = exec_pipeline_run(pipeline);
        if (block.num_rows == 0)
            break;
        for (size_t r = 0; r < block.num_rows; r++) {
            groups_count++;
            // group_key is block.columns[0].data[r]
            // sum_val is block.columns[1].data[r]
        }
        free((void *)block.columns[0].data);
        free((void *)block.columns[1].data);
        free(block.columns);
    }
    physical_operator_destroy(pipeline);
    uint64_t t1 = benchmark_now_ns();

    assert(groups_count == 4);
    (void)groups_count;
    return (double)(t1 - t0) / 1e6;
}

int main(void) {
    const char *data_dir = "/tmp/bench_simpledb_string_group_by";
    char mkdir_cmd[256];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "rm -rf %s && mkdir -p %s", data_dir, data_dir);
    system(mkdir_cmd);

    printf("=== simpledb Query B (String GROUP BY) Benchmark (10,000,000 Rows) ===\n\n");
    printf("[1/3] Generating 10M-row dataset (status StringDict + amount Int32)...\n");

    table_t *table = table_create("events");
    assert(table != NULL);
    table->schema = schema_registry_create();
    assert(table->schema != NULL);
    schema_registry_add_column(table->schema, "status", COL_TYPE_STRING_DICT);
    schema_registry_add_column(table->schema, "amount", COL_TYPE_I32);
    table->column_buffers = calloc(2, sizeof(void *));
    assert(table->column_buffers != NULL);

    string_dict_column_t *status_col = malloc(sizeof(string_dict_column_t));
    status_col->dict = string_dict_create();
    string_dict_insert(status_col->dict, "SUCCESS");
    string_dict_insert(status_col->dict, "PENDING");
    string_dict_insert(status_col->dict, "FAILED");
    string_dict_insert(status_col->dict, "REFUNDED");
    status_col->keys_buf = buffer_i32_create(NUM_ROWS);
    status_col->keys_buf->size = NUM_ROWS;
    for (size_t i = 0; i < NUM_ROWS; i++) {
        status_col->keys_buf->data[i] = (int32_t)(i % 4);
    }
    table->column_buffers[0] = status_col;

    buffer_i32_t *amount_buf = buffer_i32_create(NUM_ROWS);
    amount_buf->size = NUM_ROWS;
    for (size_t i = 0; i < NUM_ROWS; i++) {
        amount_buf->data[i] = (int32_t)((i % 1000) + 1);
    }
    table->column_buffers[1] = amount_buf;
    table->row_count = NUM_ROWS;

    int rc = table_flush(table, data_dir);
    assert(rc == 0);
    assert(table->num_parts == 1);
    (void)rc;
    printf("Flushed part created at: %s\n", table->parts[0]->part_path);

    const char *q_sql = "SELECT status, SUM(amount) FROM events GROUP BY status;";

    printf("[2/3] Warming up REPL SQL Parser, Query Planner & Caches...\n");
    double warm = run_repl_sql_pass(table, q_sql);
    printf("Warmup run took: %6.2f ms\n", warm);

    printf("[3/3] Running 3 timed REPL benchmark iterations: \"%s\"...\n\n", q_sql);
    for (int i = 0; i < 3; i++) {
        double t = run_repl_sql_pass(table, q_sql);
        printf("Iteration %d: Full REPL SQL Execution = %6.2f ms\n", i + 1, t);
    }

    table_destroy(table);
    return 0;
}
