#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "simpledb/schema_registry.h"

#define GRANULE_SIZE 8192
#define MAX_INDEXES_PER_COLUMN 8

typedef struct index_writer_t index_writer_t;

struct index_writer_t {
    void (*append_fn)(index_writer_t* self, const void* data, size_t num_rows, uint64_t current_row_offset);
    void (*close_fn)(index_writer_t* self);
    void* state;
};

typedef struct {
    const char* column_name;
    column_type_t type;

    FILE* bin_fp;
    uint64_t rows_written;

    index_writer_t* indexes[MAX_INDEXES_PER_COLUMN];
    size_t num_indexes;
} column_writer_t;

column_writer_t* column_writer_create(const char* part_dir, const char* col_name, column_type_t type);
void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows);
void column_writer_close(column_writer_t* writer);
void column_writer_attach_index(column_writer_t* writer, struct index_writer_t* index);

#include "simpledb/mmap_file.h"
#include "simpledb/slice.h"
#include "simpledb/bitmap.h"

typedef struct {
    mmap_file_t* mmap;
    column_type_t type;
    const bitmap_t* mask;
    size_t total_granules;
    size_t current_granule_idx;
} column_reader_t;

column_reader_t* column_reader_create(const char* part_dir, const char* col_name, column_type_t type, const bitmap_t* mask);
slice_i32_t column_reader_next_granule_i32(column_reader_t* reader);
void column_reader_free(column_reader_t* reader);
