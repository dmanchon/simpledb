#include "simpledb/storage.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

column_writer_t *column_writer_create(const char *part_dir, const char *col_name,
                                      column_type_t type) {

    column_writer_t *writer = malloc(sizeof(column_writer_t));
    if (!writer) return NULL;

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

    return writer;
}

void column_writer_append(column_writer_t* writer, const void* data, size_t num_rows) {
    if (num_rows == 0) return;

    // For now we assume all types (I32, F32, STRING_DICT) are 4 bytes.
    // In the future this should be derived from writer->type.
    size_t element_size = 4;
    size_t bytes_to_write = num_rows * element_size;
    
    fwrite(data, 1, bytes_to_write, writer->bin_fp);
    writer->rows_written += num_rows;
}

void column_writer_close(column_writer_t* writer) {
    if (writer) {
        if (writer->bin_fp) {
            fclose(writer->bin_fp);
        }
        free(writer);
    }
}
