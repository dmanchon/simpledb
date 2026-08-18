#include "simpledb/sparse_index.h"
#include "simpledb/bitmap.h"
#include "simpledb/storage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    FILE *idx_fp;
    int32_t min_val;
    int32_t max_val;
    size_t num_marks;
} sparse_index_writer_state_t;

static void sparse_index_writer_append(index_writer_t *self, const void *data, size_t num_rows,
                                       uint64_t current_row_offset) {
    sparse_index_writer_state_t *state = (sparse_index_writer_state_t *)self->state;
    const int32_t *values = (const int32_t *)data;
    if (num_rows == 0)
        return;

    // Since the data is sorted
    if (state->num_marks == 0 && current_row_offset == 0) {
        state->min_val = values[0];
    }
    state->max_val = values[num_rows - 1];

    // We can do this because data has constant size (i32), once it will become compressed or
    // we hold strings we will need to create a granule map
    size_t i = 0;
    if (current_row_offset % 8192 != 0) {
        i = 8192 - (current_row_offset % 8192); // Jump to the first boundary in this chunk
    }

    // since data is sorted
    while (i < num_rows) {
        fwrite(&values[i], sizeof(int32_t), 1, state->idx_fp);
        state->num_marks++;
        i += 8192;
    }
}

static void sparse_index_writer_close(index_writer_t *self) {
    sparse_index_writer_state_t *state = (sparse_index_writer_state_t *)self->state;
    sparse_index_file_header_t header = {.max_val = state->max_val,
                                         .min_val = state->min_val,
                                         .num_marks = state->num_marks,
                                         .magic = 0x5052494D,
                                         .version = 1};
    fseek(state->idx_fp, 0, SEEK_SET);
    fwrite(&header, sizeof(sparse_index_file_header_t), 1, state->idx_fp);

    fclose(state->idx_fp);
}

struct index_writer_t *sparse_index_writer_create(const char *part_dir, const char *col_name) {
    index_writer_t *writer = malloc(sizeof(index_writer_t));
    sparse_index_writer_state_t *state = malloc(sizeof(sparse_index_writer_state_t));
    state->max_val = 0;
    state->min_val = 0;
    state->num_marks = 0;

    char idx_file_path[1024];
    snprintf(idx_file_path, sizeof(idx_file_path), "%s/%s.idx", part_dir, col_name);

    state->idx_fp = fopen(idx_file_path, "wb");
    if (!state->idx_fp) {
        free(writer);
        free(state);
        return NULL;
    }
    fseek(state->idx_fp, sizeof(sparse_index_file_header_t), SEEK_SET);

    writer->state = state;
    writer->append_fn = sparse_index_writer_append;
    writer->close_fn = sparse_index_writer_close;

    return writer;
}

void sparse_index_free(sparse_index_t *idx) {
    free(idx->marks);
    free(idx);
}

bool sparse_index_search(const sparse_index_t *idx, int32_t target_val, bitmap_t *out_mask) {

    if (target_val < idx->header.min_val || target_val > idx->header.max_val) {
        return false;
    }

    size_t low = 0;
    size_t high = idx->header.num_marks - 1;
    size_t match_idx = 0;

    while (low <= high) {
        size_t mid = low + (high - low) / 2;

        if (idx->marks[mid] <= target_val) {
            match_idx = mid; // This is a valid candidate
            low = mid + 1;   // But check if there is a larger valid one
        } else {
            high = mid - 1;  // This mark is too big
        }
    }

    bitmap_set(out_mask, match_idx);
    return true;
}


sparse_index_t *sparse_index_load(const char *idx_file_path) {
    sparse_index_t *idx = malloc(sizeof(sparse_index_t));
    FILE *fp = fopen(idx_file_path, "rb");
    if (!fp) {
        free(idx);
        return NULL;
    }

    fread(&idx->header, sizeof(sparse_index_file_header_t), 1, fp);
    if (idx->header.magic != 0x5052494D || idx->header.version != 1) {
        fclose(fp);
        free(idx);
        return NULL;
    }

    idx->marks = malloc(sizeof(int32_t)*idx->header.num_marks);
    if (!idx->marks) {
        fclose(fp);
        free(idx);
        return NULL;
    }

    fread(idx->marks, sizeof(int32_t), idx->header.num_marks, fp);

    fclose(fp);
    return idx;
}
