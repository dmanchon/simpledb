#include "simpledb/storage.h"
#include "simpledb/bitmap.h"
#include "simpledb/mmap_file.h"
#include "simpledb/schema_registry.h"
#include "simpledb/slice.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

column_writer_t *column_writer_create(const char *part_dir, const char *col_name,
                                      column_type_t type) {
    column_writer_t *writer = malloc(sizeof(column_writer_t));
    if (!writer)
        return NULL;

    char bin_file_path[1024];
    snprintf(bin_file_path, sizeof(bin_file_path), "%s/%s.bin", part_dir, col_name);

    writer->bin_fp = fopen(bin_file_path, "wb");
    if (!writer->bin_fp) {
        free(writer);
        return NULL;
    }

    writer->column_name = col_name;
    writer->type = type;
    writer->rows_written = 0;
    writer->num_indexes = 0;

    return writer;
}

void column_writer_append(column_writer_t *writer, const void *data, size_t num_rows) {
    if (num_rows == 0)
        return;

    size_t element_size = 4;
    size_t bytes_to_write = num_rows * element_size;

    fwrite(data, 1, bytes_to_write, writer->bin_fp);

    for (size_t i = 0; i < writer->num_indexes; i++) {
        writer->indexes[i]->append_fn(writer->indexes[i], data, num_rows, writer->rows_written);
    }

    writer->rows_written += num_rows;
}

void column_writer_close(column_writer_t *writer) {
    if (writer) {
        if (writer->bin_fp) {
            fclose(writer->bin_fp);
        }
        free(writer);
    }
}

void column_writer_attach_index(column_writer_t *writer, index_writer_t *index) {
    if (writer->num_indexes >= MAX_INDEXES_PER_COLUMN) {
        fprintf(stderr, "[FATAL ERROR] Column has too many indexes\n");
        exit(EXIT_FAILURE);
    }
    writer->indexes[writer->num_indexes++] = index;
}

column_reader_t *column_reader_create(const char *part_dir, const char *col_name,
                                      column_type_t type, const bitmap_t *mask) {
    column_reader_t *reader = malloc(sizeof(column_reader_t));
    if (!reader)
        return NULL;

    char bin_file_path[1024];
    snprintf(bin_file_path, sizeof(bin_file_path), "%s/%s.bin", part_dir, col_name);

    reader->mmap = mmap_file_open(bin_file_path);
    if (!reader->mmap) {
        free(reader);
        return NULL;
    }

    reader->type = type;
    reader->mask = mask;
    reader->current_granule_idx = 0;
    size_t bytes_per_granule = 8192 * sizeof(int32_t);
    reader->total_granules = (reader->mmap->size + bytes_per_granule - 1) / bytes_per_granule;

    return reader;
}

void column_reader_free(column_reader_t *reader) {
    mmap_file_close(reader->mmap);
    free(reader);
}

slice_i32_t column_reader_next_granule_i32(column_reader_t *reader) {
    while (reader->current_granule_idx < reader->total_granules &&
           reader->mask &&
           !bitmap_is_set(reader->mask, reader->current_granule_idx)) {
        reader->current_granule_idx++;
    }

    if (reader->current_granule_idx >= reader->total_granules) {
        return slice_i32_create(NULL, 0);
    }

    const int32_t *ptr = (const int32_t *)reader->mmap->data + (reader->current_granule_idx * GRANULE_SIZE);

    size_t rows = GRANULE_SIZE;
    if (reader->current_granule_idx == reader->total_granules - 1) {
        size_t total_rows_in_file = reader->mmap->size / sizeof(int32_t);
        rows = total_rows_in_file - (reader->current_granule_idx * GRANULE_SIZE);
    }

    reader->current_granule_idx++;
    return slice_i32_create(ptr, rows);
}
