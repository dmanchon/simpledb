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
#define EXPECTED_SUM 5005000000LL

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
    int64_t total_sum = 0;
    while (true) {
        block_t block = exec_pipeline_run(pipeline);
        if (block.num_rows == 0)
            break;
        for (size_t r = 0; r < block.num_rows; r++) {
            total_sum += block.columns[0].data[r];
        }
        free((void *)block.columns[0].data);
        free(block.columns);
    }
    physical_operator_destroy(pipeline);
    uint64_t t1 = benchmark_now_ns();

    assert(total_sum == EXPECTED_SUM);
    (void)total_sum;
    return (double)(t1 - t0) / 1e6;
}

int main(void) {
    const char *data_dir = "/tmp/bench_simpledb_q1";
    char mkdir_cmd[256];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "rm -rf %s && mkdir -p %s", data_dir, data_dir);
    system(mkdir_cmd);

    printf("=== simpledb Q1 REPL Benchmark (SELECT SUM(amount) FROM events) (10,000,000 Rows) ===\n\n");
    printf("[1/3] Generating 10M-row dataset & flushing immutable ClickHouse Part...\n");

    table_t *table = table_create("events");
    assert(table != NULL);
    table->schema = schema_registry_create();
    assert(table->schema != NULL);
    schema_registry_add_column(table->schema, "amount", COL_TYPE_I32);
    table->column_buffers = calloc(1, sizeof(void *));
    assert(table->column_buffers != NULL);

    buffer_i32_t *buf = buffer_i32_create(NUM_ROWS);
    assert(buf != NULL);
    buf->size = NUM_ROWS;
    for (size_t i = 0; i < NUM_ROWS; i++) {
        buf->data[i] = (int32_t)((i % 1000) + 1);
    }
    table->column_buffers[0] = buf;
    table->row_count = NUM_ROWS;

    int rc = table_flush(table, data_dir);
    assert(rc == 0);
    assert(table->num_parts == 1);
    (void)rc;
    printf("Flushed part created at: %s\n", table->parts[0]->part_path);

    const char *q1_sql = "SELECT SUM(amount) FROM events;";

    // Warm-up runs through REPL
    printf("[2/3] Warming up REPL SQL Parser, Query Planner & Caches...\n");
    for (int i = 0; i < 2; i++) {
        run_repl_sql_pass(table, q1_sql);
    }

    // Measure 5 iterations through REPL
    printf("[3/3] Running 5 timed REPL benchmark iterations: \"%s\"...\n\n", q1_sql);
    double repl_times[5];
    double repl_sum = 0;

    for (int i = 0; i < 5; i++) {
        repl_times[i] = run_repl_sql_pass(table, q1_sql);
        repl_sum += repl_times[i];
        printf("Iteration %d: Full REPL SQL Execution = %6.2f ms\n", i + 1, repl_times[i]);
    }

    double avg_repl = repl_sum / 5.0;
    size_t total_bytes = NUM_ROWS * sizeof(int32_t);
    double repl_gbps = (total_bytes / (avg_repl / 1000.0)) / 1e9;
    double repl_mrows = (NUM_ROWS / (avg_repl / 1000.0)) / 1e6;

    printf("\n-------------------------------------------------------------\n");
    printf("RESULTS SUMMARY for simpledb via REPL (10,000,000 Rows, 40.0 MB):\n");
    printf("  SQL Query:                  \"%s\"\n", q1_sql);
    printf("  Full REPL Pipeline Latency:  %6.2f ms\n", avg_repl);
    printf("  Throughput:                  %6.2f GB/s\n", repl_gbps);
    printf("  Row Scan Rate:               %6.2f M rows/s\n", repl_mrows);
    printf("-------------------------------------------------------------\n");

    table_destroy(table);
    return 0;
}
