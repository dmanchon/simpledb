#include "simpledb/allocator.h"
#include "simpledb/mmap_file.h"
#include "simpledb/benchmark.h"
#include <stdio.h>
#include <stdlib.h>

static void benchmark_simd_unrolled_scan(size_t num_elements) {
    buffer_i32_t *buf = buffer_i32_create(num_elements);
    if (!buf) return;
    buf->size = num_elements;

    for (size_t i = 0; i < num_elements; i++) {
        buf->data[i] = (int32_t)(i & 0xFF);
    }

    size_t total_bytes = num_elements * sizeof(int32_t);
    uint64_t start = benchmark_now_ns();

    int64_t sum0 = 0, sum1 = 0, sum2 = 0, sum3 = 0;
    const int32_t *data = buf->data;
    size_t i = 0;

    // 4-Way SIMD Loop Unrolling for maximum Instruction-Level Parallelism (ILP)
    for (; i + 3 < num_elements; i += 4) {
        sum0 += data[i + 0];
        sum1 += data[i + 1];
        sum2 += data[i + 2];
        sum3 += data[i + 3];
    }
    for (; i < num_elements; i++) {
        sum0 += data[i];
    }
    int64_t total_sum = sum0 + sum1 + sum2 + sum3;

    // Prevent compiler Dead Code Elimination (DCE) AFTER loop without degrading vectorization!
    PREVENT_DCE(total_sum);

    uint64_t end = benchmark_now_ns();
    uint64_t elapsed_ns = end - start;

    benchmark_report("4-Way Unrolled SIMD Scan", total_bytes, elapsed_ns);

    buffer_i32_free(buf);
}

int main(void) {
    printf("=====================================================\n");
    printf("   simpledb Day 4 Micro-Benchmark Hardware Scanner   \n");
    printf("=====================================================\n\n");

    printf("[Running 10M Element Scan (40 MB Buffer)]...\n");
    benchmark_simd_unrolled_scan(10000000);

    printf("\n[Running 50M Element Scan (200 MB Buffer)]...\n");
    benchmark_simd_unrolled_scan(50000000);

    printf("\n[Running 100M Element Scan (400 MB Buffer)]...\n");
    benchmark_simd_unrolled_scan(100000000);

    printf("\n=====================================================\n");
    printf("Benchmark suite completed successfully.\n");
    return 0;
}
