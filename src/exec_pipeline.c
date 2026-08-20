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
