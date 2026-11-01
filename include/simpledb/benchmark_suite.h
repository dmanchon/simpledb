#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *query_id;
    const char *description;
    const char *sql_query;
    size_t total_rows;
    size_t total_bytes_scanned;
    double latency_ms_p50;
    double latency_ms_p99;
    double throughput_gbps;
    double throughput_mrows_sec;
    size_t peak_rss_bytes;
} benchmark_result_t;

typedef struct {
    const char *db_path;
    size_t row_count;
    size_t warmup_iterations;
    size_t measure_iterations;
    bool drop_os_cache;
} benchmark_config_t;

// Benchmark lifecycle
benchmark_config_t benchmark_config_default(const char *db_path, size_t row_count);
void benchmark_generate_dataset(const benchmark_config_t *config);
benchmark_result_t benchmark_run_query(const benchmark_config_t *config, const char *query_id, const char *sql);
void benchmark_print_summary(const benchmark_result_t *results, size_t count);

#ifdef __cplusplus
}
#endif
