#include "simpledb/db.h"
#include "simpledb/allocator.h"
#include "simpledb/slice.h"
#include "simpledb/mmap_file.h"
#include "simpledb/filter.h"
#include "simpledb/string_dict.h"
#include "simpledb/bitmap.h"
#include "simpledb/schema_registry.h"
#include "simpledb/simd_kernels.h"
#include "simpledb/storage.h"
#include "simpledb/sparse_index.h"
#include "simpledb/fts.h"
#include "simpledb/table.h"
#include "simpledb/tokenizer.h"
#include "simpledb/deletion_vector.h"
#include "simpledb/external_sort.h"
#include "simpledb/grace_hash_agg.h"
#include "simpledb/table_scanner.h"
#include "simpledb/exec_pipeline.h"
#include "simpledb/part_merger.h"
#include "simpledb/sql_parser.h"
#include "simpledb/repl.h"
#include <time.h>
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_allocator(void) {
    void* ptr = aligned_malloc(1024);
    assert(ptr != NULL);
    assert(((uintptr_t)ptr % 64) == 0 && "Memory must be 64-byte aligned");
    aligned_free(ptr);

    buffer_i32_t* buf = buffer_i32_create(4);
    assert(buf != NULL);
    assert(((uintptr_t)buf->data % 64) == 0 && "Buffer array must be 64-byte aligned");
    assert(buf->size == 0);
    assert(buf->capacity == 4);

    for (int i = 0; i < 1000; i++) {
        bool ok = buffer_i32_push(buf, i * 10);
        assert(ok);
        (void)ok;
    }

    assert(buf->size == 1000);
    assert(buf->capacity >= 1000);
    assert(buf->data[500] == 5000);

    buffer_i32_free(buf);
}

static void test_slices(void) {
    // 1. Test NULL pointer slice creation via slice_of
    slice_i32_t null_slice = slice_of((int32_t*)NULL, 3);
    assert(null_slice.data == NULL);
    assert(null_slice.len == 3);
    (void)null_slice;

    // 2. Test buffer conversion & element lookup
    buffer_i32_t* buf = buffer_i32_create(16);
    for (int i = 0; i < 10; i++) {
        buffer_i32_push(buf, (i + 1) * 100);
    }

    slice_i32_t s_buf = slice_of(buf, 0);
    assert(s_buf.len == 10);
    assert(slice_i32_at(s_buf, 0) == 100);
    assert(slice_i32_at(s_buf, 4) == 500);
    (void)s_buf;

    // 3. Test O(1) sub-slicing
    slice_i32_t sub = slice_i32_sub(s_buf, 2, 5);
    assert(sub.len == 5);
    assert(slice_i32_at(sub, 0) == 300);
    assert(slice_i32_at(sub, 4) == 700);
    (void)sub;

    // 4. Test C11 _Generic macro dispatch for int32_t vs float arrays
    int32_t raw_i32[] = { 10, 20, 30 };
    float   raw_f32[] = { 1.5f, 2.5f, 3.5f };

    slice_i32_t gen_i32 = slice_of(raw_i32, 3);
    slice_f32_t gen_f32 = slice_of(raw_f32, 3);

    assert(gen_i32.len == 3 && slice_i32_at(gen_i32, 1) == 20);
    assert(gen_f32.len == 3 && slice_f32_at(gen_f32, 1) == 2.5f);
    (void)gen_i32;
    (void)gen_f32;

    // 5. Test buffer zero-copy re-interpretation as float slice
    slice_f32_t f32_from_buf = buffer_f32_as_slice(buf);
    assert(f32_from_buf.len == 10);
    assert(f32_from_buf.data != NULL);
    (void)f32_from_buf;

    buffer_i32_free(buf);
}

static void test_mmap_file(void) {
    const char *tmp_filename = "test_column.bin";

    // Write 100 integers to temporary column file
    FILE *f = fopen(tmp_filename, "wb");
    assert(f != NULL);
    for (int32_t i = 0; i < 100; i++) {
        int32_t val = (i + 1) * 7;
        fwrite(&val, sizeof(int32_t), 1, f);
    }
    fclose(f);

    // Open via zero-copy mmap
    mmap_file_t *mfile = mmap_file_open(tmp_filename);
    assert(mfile != NULL);
    assert(mfile->size == 100 * sizeof(int32_t));
    assert(mfile->data != NULL);

    // Convert to typed slice
    slice_i32_t slice = mmap_file_as_slice_i32(mfile);
    assert(slice.len == 100);
    assert(slice_i32_at(slice, 0) == 7);
    assert(slice_i32_at(slice, 49) == 350);
    assert(slice_i32_at(slice, 99) == 700);
    (void)slice;

    // Close and remove file
    mmap_file_close(mfile);
    remove(tmp_filename);
}

static void test_filter_and_gather(void) {
    // 1. Create a 1,000-element column slice
    buffer_i32_t* salary_buf = buffer_i32_create(1000);
    for (int i = 0; i < 1000; i++) {
        buffer_i32_push(salary_buf, i * 10); // 0, 10, 20, 30, ... 9990
    }
    slice_i32_t salary_slice = buffer_i32_as_slice(salary_buf);

    // 2. Filter rows where salary > 5000 (i.e. i > 500 -> 499 matching rows)
    selection_vector_t* sel = selection_vector_create(16); // Starts small, tests auto-grow
    bool filter_ok = filter_gt_i32(salary_slice, 5000, sel);
    assert(filter_ok);
    (void)filter_ok;

    assert(sel->size == 499);
    assert(selection_vector_at(sel, 0) == 501); // Row 501 -> 5010
    assert(selection_vector_at(sel, 498) == 999); // Row 999 -> 9990

    // 3. Gather selected rows into output buffer
    buffer_i32_t* gathered_buf = buffer_i32_create(16);
    bool gather_ok = gather_i32(salary_slice, sel, gathered_buf);
    assert(gather_ok);
    (void)gather_ok;

    assert(gathered_buf->size == 499);
    assert(gathered_buf->data[0] == 5010);
    assert(gathered_buf->data[498] == 9990);

    // 4. Test chained filtering (filter_gt_i32_sel)
    selection_vector_t* sel2 = selection_vector_create(16);
    // Filter the already selected rows (which are 5010 to 9990). Find rows > 9000.
    // 5010..9000 is 400 rows. 9010..9990 is 99 rows.
    bool chained_ok = filter_gt_i32_sel(salary_slice, 9000, sel, sel2);
    assert(chained_ok);
    (void)chained_ok;

    assert(sel2->size == 99);
    assert(selection_vector_at(sel2, 0) == 901); // Row 901 -> 9010
    assert(selection_vector_at(sel2, 98) == 999); // Row 999 -> 9990

    // Cleanup
    buffer_i32_free(salary_buf);
    selection_vector_free(sel);
    selection_vector_free(sel2);
    buffer_i32_free(gathered_buf);
}

void test_string_dict(void) {
    // 1. New dict & basic insertion
    string_dict_t* d = string_dict_create();
    int32_t k;

    k = string_dict_insert(d, "SUCCESS");
    assert(k == 0);
    k = string_dict_insert(d, "FAILURE");
    assert(k == 1);
    k = string_dict_insert(d, "SUCCESS"); // Deduplicated!
    assert(k == 0);
    k = string_dict_lookup(d, "SUCCESS");
    assert(k == 0);
    (void)k;

    // 2. Bulk encode a mock column
    const char** strings = malloc(4 * sizeof(char*));
    strings[0] = "SUCCESS";
    strings[1] = "FAILURE";
    strings[2] = "SUCCESS";
    strings[3] = "TIMEOUT";

    buffer_i32_t* out_keys = buffer_i32_create(4);
    string_dict_encode_column(d, strings, 4, out_keys);

    assert(out_keys->size == 4);
    assert(out_keys->data[0] == 0); // SUCCESS
    assert(out_keys->data[2] == 0); // SUCCESS

    // 3. Filter the encoded column (WHERE status = 'SUCCESS')
    selection_vector_t* sel = selection_vector_create(16);
    slice_i32_t state_slice = buffer_i32_as_slice(out_keys);
    filter_eq_i32(state_slice, 0, sel);

    assert(sel->size == 2); // Row 0 and Row 2 matched!

    // 4. Gather & Decode (Late Materialization)
    const char** out_strings = NULL; // Must be initialized
    gather_i32_decode(state_slice, d, sel, &out_strings); // Pass address!

    assert(strcmp(out_strings[0], "SUCCESS") == 0);
    assert(strcmp(out_strings[1], "SUCCESS") == 0);

    // 5. Cleanup memory
    free(strings);
    free(out_strings);
    buffer_i32_free(out_keys);
    selection_vector_free(sel);
    string_dict_free(d);
}

void test_bitmap(void) {
    printf("Testing Bitmap...\n");
    bitmap_t* bmp = bitmap_create(8); // 8 bytes = 64 bits

    // Initially everything should be NOT NULL (0) if zero-initialized
    assert(bitmap_is_set(bmp, 5) == false);

    bitmap_set(bmp, 5);
    assert(bitmap_is_set(bmp, 5) == true);
    assert(bitmap_is_set(bmp, 4) == false);

    bitmap_clear(bmp, 5);
    assert(bitmap_is_set(bmp, 5) == false);

    bitmap_free(bmp);
    printf("Bitmap tests passed!\n");
}

void test_schema_registry(void) {
    printf("Testing Schema Registry...\n");
    schema_registry_t* reg = schema_registry_create();

    int32_t idx1 = schema_registry_add_column(reg, "user_id", COL_TYPE_I32);
    int32_t idx2 = schema_registry_add_column(reg, "status", COL_TYPE_STRING_DICT);

    assert(idx1 == 0);
    assert(idx2 == 1);
    assert(reg->count == 2);
    (void)idx1;
    (void)idx2;

    int32_t lookup_idx = schema_registry_get_column_index(reg, "status");
    assert(lookup_idx == 1);
    (void)lookup_idx;

    int32_t missing_idx = schema_registry_get_column_index(reg, "salary");
    assert(missing_idx == -1);
    (void)missing_idx;

    schema_registry_free(reg);
    printf("Schema Registry tests passed!\n");
}

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000.0) + (ts.tv_nsec / 1000000.0);
}

void test_simd_primitives(void) {
    printf("Testing SIMD APL Primitives (4 Million Elements)...\n");
    const size_t NUM_ELEMENTS = 4000000;

    int32_t* src = aligned_malloc(NUM_ELEMENTS * sizeof(int32_t));
    uint8_t* mask = aligned_malloc(NUM_ELEMENTS * sizeof(uint8_t));
    int32_t* dst = aligned_malloc(NUM_ELEMENTS * sizeof(int32_t));

    assert(src && mask && dst);

    // Generate random data
    for (size_t i = 0; i < NUM_ELEMENTS; i++) {
        src[i] = rand() % 1000;
        mask[i] = (rand() % 100) < 25 ? 1 : 0; // 25% selectivity
    }

    double start = get_time_ms();
    size_t written = simd_compress_i32(src, mask, dst, NUM_ELEMENTS);
    double end = get_time_ms();

    printf("Compress extracted %zu elements in %.3f ms\n", written, end - start);

    start = get_time_ms();
    int64_t total_sum = simd_sum_i32(dst, written);
    end = get_time_ms();

    printf("Sum Reduction (Total: %" PRId64 ") took %.3f ms\n", total_sum, end - start);

    aligned_free(src);
    aligned_free(mask);
    aligned_free(dst);

    printf("SIMD Primitives tests passed!\n");
}

void test_column_writer(void) {
    printf("Testing Column Writer...\n");
    // Ensure test directory exists or just write to current directory
    const char* test_dir = ".";
    const char* col_name = "test_col";

    column_writer_t* writer = column_writer_create(test_dir, col_name, COL_TYPE_I32);
    assert(writer != NULL);
    assert(writer->rows_written == 0);

    // Create 10,000 ints
    size_t num_rows = 10000;
    int32_t* data = malloc(num_rows * sizeof(int32_t));
    for (size_t i = 0; i < num_rows; i++) {
        data[i] = (int32_t)i;
    }

    // Append in chunks
    column_writer_append(writer, data, 5000); // chunk 1
    column_writer_append(writer, data + 5000, 5000); // chunk 2

    assert(writer->rows_written == 10000);

    column_writer_close(writer);
    free(data);

    // Verify file size
    FILE* fp = fopen("./test_col.bin", "rb");
    assert(fp != NULL);
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    (void)size;

    assert(size == 40000); // 10,000 * 4 bytes

    // Cleanup file
    remove("./test_col.bin");

    printf("Column Writer tests passed!\n");
}

void test_sparse_index_e2e(void) {
    printf("Testing Sparse Index E2E...\n");
    const char* tmp_dir = ".";
    const char* col_name = "test_col_idx";

    // 1. Create the writer
    index_writer_t* writer = sparse_index_writer_create(tmp_dir, col_name);
    assert(writer != NULL);

    // 2. Append 24,576 rows (exactly 3 Granules)
    int32_t chunk[8192];
    uint64_t row_offset = 0;

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 8192; j++) {
            chunk[j] = row_offset + j;
        }
        // Feed chunk into the index
        writer->append_fn(writer, chunk, 8192, row_offset);
        row_offset += 8192;
    }

    // 3. Finalize and close the file (writes the header!)
    writer->close_fn(writer);
    free(writer); // The factory malloc'd the index_writer_t struct

    // 4. Read Phase: Load the index back from disk
    char idx_path[256];
    snprintf(idx_path, sizeof(idx_path), "%s/%s.idx", tmp_dir, col_name);

    sparse_index_t* idx = sparse_index_load(idx_path);
    assert(idx != NULL);
    assert(idx->header.num_marks == 3);
    assert(idx->header.min_val == 0);
    assert(idx->header.max_val == 24575);

    // 5. Search Phase
    bitmap_t* mask = bitmap_create(1); // 1 byte holds 8 bits

    // Search for 10000, which falls in Granule 1 (8192 to 16383)
    bool found = sparse_index_search(idx, 10000, mask);
    assert(found == true);
    (void)found;
    assert(bitmap_is_set(mask, 1) == true);
    assert(bitmap_is_set(mask, 0) == false);

    // 6. Cleanup
    bitmap_free(mask);
    sparse_index_free(idx);
    remove(idx_path); // Clean up the temp file

    printf("Sparse Index tests passed!\n");
}

void test_column_reader(void) {
    // 1. Write a dummy file with exactly 3 Granules (24,576 rows)
    const char* col_name = "reader_test";
    column_writer_t* writer = column_writer_create(".", col_name, COL_TYPE_I32);
    int32_t* data = malloc(24576 * sizeof(int32_t));
    for(int i = 0; i < 24576; i++) data[i] = i; // 0 to 24575
    column_writer_append(writer, data, 24576);
    column_writer_close(writer);
    free(data);

    // 2. Create a Bitmap that ONLY selects Granule 1
    bitmap_t* mask = bitmap_create(1);
    bitmap_set(mask, 1);

    // 3. Open the Reader
    column_reader_t* reader = column_reader_create(".", col_name, COL_TYPE_I32, mask);

    // 4. Fetch the next Granule.
    // It should SKIP Granule 0 and instantly return Granule 1!
    slice_i32_t slice = column_reader_next_granule_i32(reader);

    assert(slice.len == 8192);
    // The first element of Granule 1 should be 8192
    assert(slice_i32_at(slice, 0) == 8192);

    // 5. Fetch again. Granule 2 is masked out, so it should hit EOF!
    slice_i32_t empty = column_reader_next_granule_i32(reader);
    assert(empty.len == 0);

    // 6. Cleanup
    column_reader_free(reader);
    bitmap_free(mask);
    remove("./reader_test.bin");

    printf("Column Reader tests passed!\n");
}

void test_raw_strings(void) {
    const char* col_name = "string_test";
    column_writer_t* writer = column_writer_create(".", col_name, COL_TYPE_STRING);

    const char* data[3] = {
        "apple",
        "banana",
        "cherry"
    };

    column_writer_append(writer, data, 3);
    column_writer_close(writer);

    // 3. Open the Reader
    column_reader_t* reader = column_reader_create(".", col_name, COL_TYPE_STRING, NULL);
    slice_i32_t slice = column_reader_next_granule_i32(reader);

    assert(slice.len == 3);

    assert(slice_i32_at(slice, 0) == 0);
    assert(slice_i32_at(slice, 1) == 6);
    assert(slice_i32_at(slice, 2) == 13);

    const char* strings_data = (const char*)reader->strings_mmap->data;
    assert(strcmp(strings_data + 0, "apple") == 0);
    assert(strcmp(strings_data + 6, "banana") == 0);
    assert(strcmp(strings_data + 13, "cherry") == 0);

    // 6. Cleanup
    column_reader_free(reader);
    remove("./string_test.bin");
    remove("./string_test.strings");

    printf("Raw string tests passed!\n");
}

void test_tokenizer(void) {
    printf("Testing Zero-Copy Tokenizer...\n");

    const char* text = "  Hello, World!!!  This is state-of-the-art... don't you agree?  ";
    tokenizer_t t = tokenizer_create(text, strlen(text));
    token_t tok;

    // 1. "Hello"
    assert(tokenizer_next(&t, &tok) == 1);
    assert(tok.len == 5 && strncmp(tok.text, "Hello", 5) == 0);

    // 2. "World"
    assert(tokenizer_next(&t, &tok) == 1);
    assert(tok.len == 5 && strncmp(tok.text, "World", 5) == 0);

    // 3. "This"
    assert(tokenizer_next(&t, &tok) == 1);
    assert(tok.len == 4 && strncmp(tok.text, "This", 4) == 0);

    // 4. "is"
    assert(tokenizer_next(&t, &tok) == 1);
    assert(tok.len == 2 && strncmp(tok.text, "is", 2) == 0);

    // 5. "state-of-the-art" (Testing hyphen whitelist!)
    assert(tokenizer_next(&t, &tok) == 1);
    assert(tok.len == 16 && strncmp(tok.text, "state-of-the-art", 16) == 0);

    // 6. "don't" (Testing apostrophe whitelist!)
    assert(tokenizer_next(&t, &tok) == 1);
    assert(tok.len == 5 && strncmp(tok.text, "don't", 5) == 0);

    // 7. "you"
    assert(tokenizer_next(&t, &tok) == 1);

    // 8. "agree"
    assert(tokenizer_next(&t, &tok) == 1);

    // 9. EOF!
    assert(tokenizer_next(&t, &tok) == 0);

    printf("Tokenizer tests passed!\n");
}

void test_inverted_index(void) {
    printf("Testing FTS Inverted Index...\n");

    inverted_index_t* idx = inverted_index_create();

    // Simulate Granule 0
    inverted_index_add_document(idx, "database connection timeout", 0);
    inverted_index_add_document(idx, "timeout occurred", 0);

    // Simulate Granule 1
    inverted_index_add_document(idx, "query executed successfully", 1);

    // Simulate Granule 2 (Testing case-insensitivity with TIMEOUT)
    inverted_index_add_document(idx, "TIMEOUT while waiting for connection", 2);

    // 1. Search for "timeout" (Should exist in Granules 0 and 2)
    bitmap_t* result = inverted_index_search(idx, "timeout");
    assert(result != NULL);
    assert(bitmap_is_set(result, 0) == true);
    assert(bitmap_is_set(result, 1) == false);
    assert(bitmap_is_set(result, 2) == true);

    // 2. Search for "connection" (Should exist in Granules 0 and 2)
    result = inverted_index_search(idx, "connection");
    assert(result != NULL);
    assert(bitmap_is_set(result, 0) == true);
    assert(bitmap_is_set(result, 1) == false);
    assert(bitmap_is_set(result, 2) == true);

    // 3. Search for "query" (Should exist only in Granule 1)
    result = inverted_index_search(idx, "query");
    assert(result != NULL);
    assert(bitmap_is_set(result, 0) == false);
    assert(bitmap_is_set(result, 1) == true);
    assert(bitmap_is_set(result, 2) == false);

    // 4. Search for "error" (Does not exist anywhere)
    result = inverted_index_search(idx, "error");
    assert(result == NULL);

    inverted_index_free(idx);
    printf("FTS tests passed!\n");
}

void test_table_creation(void) {
    table_t* t = table_create("events");
    assert(t != NULL);
    assert(strcmp(t->name, "events") == 0);
    assert(t->row_count == 0);

    table_destroy(t);
    printf("test_table_creation passed.\n");
}

void test_table_e2e_batch_ingestion(void) {
    printf("Testing Table E2E Batch Ingestion...\n");

    // 1. Create table & setup schema
    table_t* table = table_create("user_events");
    assert(table != NULL);

    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "score", COL_TYPE_F32);
    schema_registry_add_column(schema, "country", COL_TYPE_STRING_DICT);
    schema_registry_add_column(schema, "comment", COL_TYPE_STRING);
    table->schema = schema;

    // Allocate column buffers
    table->column_buffers = calloc(schema->count, sizeof(void*));
    table->column_buffers[0] = buffer_i32_create(4);
    table->column_buffers[1] = buffer_i32_create(4);

    string_dict_column_t* sdc = malloc(sizeof(string_dict_column_t));
    sdc->dict = string_dict_create();
    sdc->keys_buf = buffer_i32_create(4);
    table->column_buffers[2] = sdc;

    table->column_buffers[3] = raw_string_column_create(4);

    // 2. Batch 1: Ingest 3 rows
    int32_t b1_user_ids[] = { 101, 102, 103 };
    float   b1_scores[]   = { 95.5f, 82.0f, 99.1f };
    const char* b1_countries[] = { "US", "ES", "US" };
    const char* b1_comments[]  = { "First event", "Second event", "Third event" };

    const void* batch1[4];
    batch1[0] = b1_user_ids;
    batch1[1] = b1_scores;
    batch1[2] = b1_countries;
    batch1[3] = b1_comments;

    int rc = table_insert_batch(table, (const void***)batch1, 4, 3);
    assert(rc == 0);
    assert(table->row_count == 3);

    // 3. Batch 2: Ingest 2 more rows to test offset appending & buffer growth
    int32_t b2_user_ids[] = { 104, 105 };
    float   b2_scores[]   = { 77.4f, 88.9f };
    const char* b2_countries[] = { "FR", "ES" };
    const char* b2_comments[]  = { "Fourth event", "Fifth event" };

    const void* batch2[4];
    batch2[0] = b2_user_ids;
    batch2[1] = b2_scores;
    batch2[2] = b2_countries;
    batch2[3] = b2_comments;

    rc = table_insert_batch(table, (const void***)batch2, 4, 2);
    assert(rc == 0);
    assert(table->row_count == 5);

    // 4. Verify Column 0 (I32 user_id)
    buffer_i32_t* c0 = (buffer_i32_t*)table->column_buffers[0];
    assert(c0->size == 5);
    assert(c0->data[0] == 101 && c0->data[4] == 105);

    // 5. Verify Column 1 (F32 score)
    buffer_i32_t* c1 = (buffer_i32_t*)table->column_buffers[1];
    assert(c1->size == 5);
    assert(((float*)c1->data)[0] == 95.5f);
    assert(((float*)c1->data)[4] == 88.9f);

    // 6. Verify Column 2 (STRING_DICT country)
    string_dict_column_t* c2 = (string_dict_column_t*)table->column_buffers[2];
    assert(c2->keys_buf->size == 5);
    assert(c2->keys_buf->data[0] == 0); // "US"
    assert(c2->keys_buf->data[1] == 1); // "ES"
    assert(c2->keys_buf->data[2] == 0); // "US" (deduplicated to 0)
    assert(c2->keys_buf->data[3] == 2); // "FR"
    assert(c2->keys_buf->data[4] == 1); // "ES" (deduplicated to 1)

    // 7. Verify Column 3 (Raw String comment)
    raw_string_column_t* c3 = (raw_string_column_t*)table->column_buffers[3];
    assert(c3->size == 5);
    assert(strcmp(c3->strings[0], "First event") == 0);
    assert(strcmp(c3->strings[4], "Fifth event") == 0);

    // 8. Teardown
    table_destroy(table);
    printf("Table E2E Batch Ingestion tests passed!\n");
}

static void test_table_flush_e2e(void) {
    printf("Testing Table Flush E2E (Disk Serialization & Buffer Reset)...\n");

    // 1. Create table & setup 4-column schema
    table_t *table = table_create("events_flush");
    assert(table != NULL);

    schema_registry_t *schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "score", COL_TYPE_F32);
    schema_registry_add_column(schema, "country", COL_TYPE_STRING_DICT);
    schema_registry_add_column(schema, "comment", COL_TYPE_STRING);
    table->schema = schema;

    table->column_buffers = calloc(schema->count, sizeof(void *));
    table->column_buffers[0] = buffer_i32_create(4);
    table->column_buffers[1] = buffer_i32_create(4);

    string_dict_column_t *sdc = malloc(sizeof(string_dict_column_t));
    sdc->dict = string_dict_create();
    sdc->keys_buf = buffer_i32_create(4);
    table->column_buffers[2] = sdc;

    table->column_buffers[3] = raw_string_column_create(4);

    // 2. Ingest batch of 3 rows
    int32_t user_ids[] = { 201, 202, 203 };
    float scores[] = { 10.5f, 20.0f, 30.5f };
    const char *countries[] = { "US", "UK", "US" };
    const char *comments[] = { "alpha", "beta", "gamma" };

    const void *batch[4];
    batch[0] = user_ids;
    batch[1] = scores;
    batch[2] = countries;
    batch[3] = comments;

    int rc_batch = table_insert_batch(table, (const void ***)batch, 4, 3);
    assert(rc_batch == 0);
    assert(table->row_count == 3);

    // 3. Flush in-memory data to disk
    int rc_flush2 = table_flush(table, ".");
    assert(rc_flush2 == 0);

    // 4. Assert in-memory buffer was reset
    assert(table->row_count == 0);
    buffer_i32_t *c0 = (buffer_i32_t *)table->column_buffers[0];
    assert(c0->size == 0);
    assert(c0->capacity >= 4); // Capacity retained!
    (void)c0;

    // 5. Assert part catalog registered the new immutable part
    assert(table->num_parts == 1);
    assert(table->parts[0]->row_count == 3);

    // 6. Test Read-Path Metadata APIs
    size_t active_rows = 999;
    void **active_bufs = table_get_active_buffer(table, &active_rows);
    assert(active_bufs != NULL && active_rows == 0);
    (void)active_bufs;

    size_t num_parts = 0;
    table_part_t **parts = table_get_parts(table, &num_parts);
    assert(parts != NULL && num_parts == 1);
    assert(parts[0]->row_count == 3);
    (void)parts;

    // 7. Verify serialized files exist on disk inside the part directory
    FILE *f_user_id = fopen("./all_1_1_0/user_id.bin", "rb");
    assert(f_user_id != NULL);
    fclose(f_user_id);

    FILE *f_country_dict = fopen("./all_1_1_0/country.dict", "rb");
    assert(f_country_dict != NULL);
    fclose(f_country_dict);

    FILE *f_comment_strings = fopen("./all_1_1_0/comment.strings", "rb");
    assert(f_comment_strings != NULL);
    fclose(f_comment_strings);

    // Cleanup generated disk files
    remove("./all_1_1_0/user_id.bin");
    remove("./all_1_1_0/user_id.idx");
    remove("./all_1_1_0/score.bin");
    remove("./all_1_1_0/score.idx");
    remove("./all_1_1_0/country.bin");
    remove("./all_1_1_0/country.idx");
    remove("./all_1_1_0/country.dict");
    remove("./all_1_1_0/comment.bin");
    remove("./all_1_1_0/comment.idx");
    remove("./all_1_1_0/comment.strings");
    remove("./all_1_1_0");

    table_destroy(table);
    printf("Table Flush E2E tests passed!\n");
}

void test_deletion_vector(void) {
    printf("Testing Deletion Vector...\n");

    // 1. Sparse test
    deletion_vector_t *dv = deletion_vector_create(10);
    deletion_vector_add(dv, 5);
    deletion_vector_add(dv, 1000000);
    assert(deletion_vector_is_deleted(dv, 5) == true);
    assert(deletion_vector_is_deleted(dv, 1000000) == true);
    assert(deletion_vector_is_deleted(dv, 6) == false);
    deletion_vector_free(dv);

    // 2. Dense upgrade test
    dv = deletion_vector_create(10);
    for (uint32_t i = 0; i < 5000; i++) {
        deletion_vector_add(dv, i);
    }
    assert(deletion_vector_is_deleted(dv, 4999) == true);
    assert(deletion_vector_is_deleted(dv, 5000) == false);
    deletion_vector_free(dv);

    // 3. SIMD mask test
    dv = deletion_vector_create(10);
    deletion_vector_add(dv, 2);
    deletion_vector_add(dv, 3);
    deletion_vector_add(dv, 63);
    uint64_t mask = deletion_vector_get_simd_mask(dv, 0);
    assert((mask & (1ULL << 2)) != 0);
    assert((mask & (1ULL << 3)) != 0);
    assert((mask & (1ULL << 63)) != 0);
    assert((mask & (1ULL << 4)) == 0);
    deletion_vector_free(dv);

    printf("Deletion Vector tests passed!\n");
}

static void test_external_sorter_in_memory(void) {
    printf("Testing External Sorter (In-Memory Fallback)...\n");

    external_sorter_t *sorter = external_sorter_create(".", 20);
    assert(sorter != NULL);

    sort_record_t input[5] = {
        { 40, 1 }, { 10, 2 }, { 50, 3 }, { 20, 4 }, { 30, 5 }
    };
    assert(external_sorter_push(sorter, input, 5) == 0);
    assert(external_sorter_finish(sorter) == 0);

    sort_record_t rec;
    int32_t expected_keys[5] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; i++) {
        assert(external_sorter_next(sorter, &rec) == true);
        assert(rec.key == expected_keys[i]);
    }
    assert(external_sorter_next(sorter, &rec) == false);

    external_sorter_destroy(sorter);
    printf("External Sorter In-Memory tests passed!\n");
}

static void test_external_sorter_disk_spilling(void) {
    printf("Testing External Sorter (Disk-Spilling K-Way Merge)...\n");

    size_t mem_limit = 100;
    size_t total_records = 1000;
    external_sorter_t *sorter = external_sorter_create(".", mem_limit);
    assert(sorter != NULL);

    sort_record_t *input = malloc(sizeof(sort_record_t) * total_records);
    for (size_t i = 0; i < total_records; i++) {
        input[i].key = (int32_t)(total_records - i); // Reverse sorted keys
        input[i].row_id = (int32_t)i;
    }

    for (size_t i = 0; i < total_records; i += 50) {
        assert(external_sorter_push(sorter, input + i, 50) == 0);
    }
    free(input);

    assert(external_sorter_finish(sorter) == 0);
    assert(sorter->num_runs == 10);

    sort_record_t rec;
    int32_t prev_key = -1;
    size_t count = 0;

    while (external_sorter_next(sorter, &rec)) {
        if (count > 0) {
            assert(rec.key >= prev_key);
        }
        prev_key = rec.key;
        count++;
    }

    assert(count == total_records);
    assert(prev_key == (int32_t)total_records);

    external_sorter_destroy(sorter);
    printf("External Sorter Disk-Spilling tests passed!\n");
}

static void test_min_heap(void) {
    printf("Testing Standalone Min-Heap Priority Queue (Key + Payload)...\n");
    min_heap_t *heap = min_heap_create(10);
    assert(heap != NULL);
    assert(heap->size == 0);

    const char *payloads[5] = { "stream_0", "stream_1", "stream_2", "stream_3", "stream_4" };

    // 1. Push elements in arbitrary unsorted sequence
    assert(min_heap_push(heap, 50, (void *)payloads[0]) == 0);
    assert(min_heap_push(heap, 20, (void *)payloads[1]) == 0);
    assert(min_heap_push(heap, 40, (void *)payloads[2]) == 0);
    assert(min_heap_push(heap, 10, (void *)payloads[3]) == 0);
    assert(min_heap_push(heap, 30, (void *)payloads[4]) == 0);

    assert(heap->size == 5);

    // 2. Peek champion at root (must be 10 without removing it)
    heap_node_t node;
    assert(min_heap_peek(heap, &node) == true);
    assert(node.key == 10);
    assert(node.payload == (void *)payloads[3]);

    // 3. Pop elements one by one - must emerge in strict ascending sorted order!
    int64_t expected_keys[5] = { 10, 20, 30, 40, 50 };
    const char *expected_payloads[5] = { "stream_3", "stream_1", "stream_4", "stream_2", "stream_0" };
    for (int i = 0; i < 5; i++) {
        assert(min_heap_pop(heap, &node) == true);
        assert(node.key == expected_keys[i]);
        assert(node.payload == (void *)expected_payloads[i]);
    }

    // 4. Verify empty heap state
    assert(heap->size == 0);
    assert(min_heap_pop(heap, &node) == false);
    assert(min_heap_peek(heap, &node) == false);

    // 5. Test min_heap_replace_top (O(log K) replacement)
    assert(min_heap_push(heap, 100, (void *)payloads[0]) == 0);
    assert(min_heap_push(heap, 200, (void *)payloads[1]) == 0);
    assert(min_heap_replace_top(heap, 300, (void *)payloads[2]) == true);
    assert(min_heap_peek(heap, &node) == true);
    assert(node.key == 200);

    min_heap_destroy(heap);
    (void)payloads;
    (void)node;
    (void)expected_keys;
    (void)expected_payloads;
    printf("Standalone Min-Heap tests passed!\n");
}

static void test_grace_hash_agg(void) {
    printf("Testing Partitioned Grace Hash Aggregation...\n");
    // Memory limit of 100 records, 4 partitions
    grace_hash_agg_t *agg = grace_hash_agg_create("test_grace_agg_tmp", 100, 4);
    assert(agg != NULL);

    size_t total_records = 500; // Will force spilling
    agg_input_record_t *records = malloc(total_records * sizeof(agg_input_record_t));
    for (size_t i = 0; i < total_records; i++) {
        records[i].group_key = i % 10; // 10 unique groups
        records[i].agg_value = 5;
    }

    assert(grace_hash_agg_push(agg, records, total_records) == 0);
    assert(grace_hash_agg_finish(agg) == 0);

    agg_output_record_t out_rec;
    size_t out_count = 0;
    while (grace_hash_agg_next(agg, &out_rec)) {
        assert(out_rec.count_value == 50);
        assert(out_rec.sum_value == 250);
        out_count++;
    }

    assert(out_count == 10); // 10 unique groups

    grace_hash_agg_destroy(agg);
    remove("test_grace_agg_tmp");
    free(records);
    printf("Grace Hash Aggregation tests passed!\n");
}

static void test_table_scanner(void) {
    printf("Testing Multi-Part Table Scanner...\n");

    // 1. Setup a table and schema
    table_t* table = table_create("metrics");
    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "cpu", COL_TYPE_I32);
    schema_registry_add_column(schema, "ram", COL_TYPE_I32);
    table->schema = schema;

    // Allocate polymorphic buffers
    table->column_buffers = calloc(2, sizeof(void*));
    table->column_buffers[0] = buffer_i32_create(10);
    table->column_buffers[1] = buffer_i32_create(10);

    // 2. Insert data into memory
    int32_t cpu_vals[] = { 10, 20, 30 };
    int32_t ram_vals[] = { 100, 200, 300 };
    const void* batch1[] = { cpu_vals, ram_vals };
    table_insert_batch(table, (const void ***)batch1, 2, 3);

    // 3. (Mock) Flush to part 0
    // In a real test, we would call table_flush(table, "."), but we assume it's mocked here
    // to create disk files, and then we insert more data to remain in memory.

    // 4. Create Scanner for only the "cpu" column (Projection Pushdown)
    const char* cols[] = { "cpu" };
    table_scanner_t* scanner = table_scanner_create(table, cols, 1);
    assert(scanner != NULL);

    // 5. Scan blocks
    size_t total_rows_scanned = 0;
    while (true) {
        block_t block = table_scanner_next(scanner);
        if (block.num_rows == 0) break;

        assert(block.num_columns == 1);
        total_rows_scanned += block.num_rows;

        // Free block slices if necessary according to your implementation
        if (block.columns) free(block.columns);
    }

    // In our mock, if we scanned the memory buffer, we should see 3 rows.
    assert(total_rows_scanned == 3);

    table_scanner_destroy(scanner);
    table_destroy(table);
    printf("Table Scanner tests passed!\n");
}

int test_exec_pipeline(void) {
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
    int32_t user_ids[] = {1,    2,   1,   3,  2,   1};
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

void test_part_merger(void) {
    // 1. Setup a Table
    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "amount", COL_TYPE_I32);
    
    table_t* table = table_create("events_merge_test");
    table->schema = schema;
    table->column_buffers = malloc(sizeof(void*) * 2);
    table->column_buffers[0] = buffer_i32_create(4);
    table->column_buffers[1] = buffer_i32_create(4);
    
    // 2. Insert Data and Flush to create Part 1 (simulate batch 1)
    int32_t users1[] = {1, 2};
    int32_t amounts1[] = {100, 200};
    const void *batch1[2];
    batch1[0] = users1;
    batch1[1] = amounts1;
    
    int rc_batch1 = table_insert_batch(table, (const void ***)batch1, 2, 2);
    assert(rc_batch1 == 0);
    int rc_flush1 = table_flush(table, ".");
    assert(rc_flush1 == 0); // Creates part 0
    
    // 3. Insert Data and Flush to create Part 2 (simulate batch 2)
    int32_t users2[] = {3, 4};
    int32_t amounts2[] = {300, 400};
    const void *batch2[2];
    batch2[0] = users2;
    batch2[1] = amounts2;
    
    int rc_batch2 = table_insert_batch(table, (const void ***)batch2, 2, 2);
    assert(rc_batch2 == 0);
    int rc_flush2 = table_flush(table, ".");
    assert(rc_flush2 == 0); // Creates part 1
    
    // Assume part 0 and part 1 are now immutable parts on disk.
    // Let's verify we have exactly 2 parts.
    assert(table->num_parts == 2);
    
    // 4. Run the Compactor
    merge_config_t config = {
        .max_parts_to_merge = 10,
        .min_rows_to_merge = 1,
        .force_merge_all = true // Force compaction of all existing parts
    };
    
    part_merger_t* merger = part_merger_create(table, config);
    if (merger->num_parts > 0) {
        part_merger_execute(merger);
    }
    part_merger_destroy(merger);
    
    // 5. Verify the Table State
    // The table should now only have 1 active, consolidated part!
    assert(table->num_parts == 1);
    
    // 6. Cleanup
    table_destroy(table);
    remove("./all_1_1_0/user_id.bin");
    remove("./all_1_1_0/amount.bin");
    remove("./all_1_1_0");
    remove("./all_2_2_0/user_id.bin");
    remove("./all_2_2_0/amount.bin");
    remove("./all_2_2_0");
    remove("events_merge_test/all_1_2_1/user_id.bin");
    remove("events_merge_test/all_1_2_1/amount.bin");
    remove("events_merge_test/all_1_2_1");
    remove("events_merge_test");
    
    printf("Day 19 MergeTree Part Compactor test passed!\n");
}

static void test_sql_parser(void) {
    // 1. Test SELECT with projection, aggregate, alias, WHERE, GROUP BY, and ORDER BY DESC
    {
        const char* query = "SELECT user_id, sum(amount) AS total FROM events WHERE amount > 100 GROUP BY user_id ORDER BY total DESC;";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};

        bool ok = sql_parser_parse(&parser, &ast);
        assert(ok);
        (void)ok;
        assert(ast.stmt_type == STMT_SELECT);

        assert(strcmp(ast.table_name, "events") == 0);
        assert(ast.num_select_exprs == 2);
        assert(ast.select_exprs[0].type == AST_PROJECTION);
        assert(strcmp(ast.select_exprs[0].column_name, "user_id") == 0);

        assert(ast.select_exprs[1].type == AST_AGG_SUM);
        assert(strcmp(ast.select_exprs[1].column_name, "amount") == 0);
        assert(strcmp(ast.select_exprs[1].alias, "total") == 0);

        assert(ast.num_predicates == 1);
        assert(strcmp(ast.predicates[0].column_name, "amount") == 0);
        assert(ast.predicates[0].op == OP_GREATER_THAN);
        assert(ast.predicates[0].int_val == 100);

        assert(ast.has_group_by);
        assert(strcmp(ast.group_by_col, "user_id") == 0);

        assert(ast.has_order_by);
        assert(strcmp(ast.order_by_col, "total") == 0);
        assert(ast.order_desc == true);
    }

    // 2. Test Single-Row INSERT ... VALUES
    {
        const char* query = "INSERT INTO events (user_id, status) VALUES (42, 'SUCCESS');";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};

        bool ok = sql_parser_parse(&parser, &ast);
        assert(ok);
        (void)ok;
        assert(ast.stmt_type == STMT_INSERT);

        assert(strcmp(ast.table_name, "events") == 0);

        assert(ast.num_insert_cols == 2);
        assert(strcmp(ast.insert_cols[0], "user_id") == 0);
        assert(strcmp(ast.insert_cols[1], "status") == 0);

        assert(ast.num_insert_vals == 2);
        assert(ast.insert_vals[0].is_string == false);
        assert(ast.insert_vals[0].int_val == 42);

        assert(ast.insert_vals[1].is_string == true);
        assert(strcmp(ast.insert_vals[1].string_val, "SUCCESS") == 0);
    }

    // 3. Test Bulk JSON INSERT ... FROM
    {
        const char* query = "INSERT INTO events FROM 'events_data.json';";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};

        bool ok = sql_parser_parse(&parser, &ast);
        assert(ok);
        (void)ok;
        assert(ast.stmt_type == STMT_INSERT);

        assert(strcmp(ast.table_name, "events") == 0);
        assert(ast.is_bulk_insert == true);
        assert(strcmp(ast.from_file_path, "events_data.json") == 0);
    }

    printf("Day 20 SQL Parser tests passed (SELECT, INSERT, & Bulk JSON)!\n");
}

static void test_repl_write_path_execution(void) {
    table_t *table = table_create("events_merge_test");

    {
	// simple test:
        const char *query = "INSERT INTO events (user_id, status) VALUES (42, 'SUCCESS');";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
        assert(table->row_count == 1);
        assert(table->schema != NULL);
        assert(table->schema->count == 2);
        // Verify physical columnar vectors in memory
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[0] == 42);
        raw_string_column_t *str_buf = (raw_string_column_t *)table->column_buffers[1];
        assert(strcmp(str_buf->strings[0], "SUCCESS") == 0);
    }
    {
	// schema evolution:
        const char *query = "INSERT INTO events (user_id, status, amount) VALUES (42, 'SUCCESS', 3.2);";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
        assert(table->row_count == 2);
        assert(table->schema != NULL);
        assert(table->schema->count == 3);
        // Verify physical columnar vectors in memory
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[0] == 42);
        raw_string_column_t *str_buf = (raw_string_column_t *)table->column_buffers[1];
        assert(strcmp(str_buf->strings[0], "SUCCESS") == 0);
    }
    table_destroy(table);
    table = table_create("events_merge_test");
    {
	char tmp_path[] = "/tmp/cdb_bulk_XXXXXX";
        int fd = mkstemp(tmp_path);
        assert(fd != -1);
        FILE *f = fdopen(fd, "w");
        assert(f != NULL);
        fprintf(f, "{\"user_id\": 101, \"status\": \"PENDING\"}\n");
        fprintf(
            f,
            "{\"user_id\": 102, \"status\": \"SUCCESS\", \"amount\": 250, \"region\": \"EU\"}\n");

        for (int i = 0; i < 15000; i++) {
            fprintf(f, "{\"user_id\": %d, \"amount\": 99, \"region\": \"EU\"}\n", i + 103);
        }
        fclose(f); // Closes both FILE* and the underlying fd
        char query[256];
        snprintf(query, sizeof(query), "INSERT INTO events FROM '%s';", tmp_path);
	
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
	assert(table->row_count == 15002);
        assert(table->schema != NULL);
        assert(table->schema->count == 4);
	// Verify physical columnar vectors in memory
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[0] == 101);
        raw_string_column_t *str_buf = (raw_string_column_t *)table->column_buffers[1];
        assert(strcmp(str_buf->strings[0], "PENDING") == 0);
    }

    printf("Day 21 repl execution write-path runtime passed!\n");
}

static void test_repl_read_path_execution(void) {
    table_t *table = table_create("events_merge_test");

    {
	// simple test add data:
	char tmp_path[] = "/tmp/cdb_bulk_XXXXXX";
        int fd = mkstemp(tmp_path);
        assert(fd != -1);
        FILE *f = fdopen(fd, "w");
        assert(f != NULL);

        for (int i = 0; i < 15000; i++) {
            fprintf(f, "{\"user_id\": %d, \"amount\": %d, \"region\": \"EU\"}\n", i, i % 5);
        }
        fclose(f); // Closes both FILE* and the underlying fd
        char query[256];
        snprintf(query, sizeof(query), "INSERT INTO events FROM '%s';", tmp_path);
	
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
	assert(table->row_count == 15000);
        assert(table->schema != NULL);
        assert(table->schema->count == 3);
	// Verify physical columnar vectors in memory
        buffer_i32_t *id_buf = (buffer_i32_t *)table->column_buffers[0];
        assert(id_buf->data[0] == 0);

	const char select_query[256] = "SELECT user_id, SUM(amount) FROM events WHERE amount < 100 GROUP BY user_id;";
        parser = sql_parser_create(select_query);
	assert(sql_parser_parse(&parser, &ast));
        physical_operator_t *pipeline = repl_exec_select(table, &ast);
        assert(pipeline != NULL);

        size_t total_rows = 0;
        size_t num_blocks = 0;
        while (true) {
            block_t block = exec_pipeline_run(pipeline);
            if (block.num_rows == 0)
                break;
            total_rows += block.num_rows;
            num_blocks++;
        }
        assert(num_blocks == 2); // Page 1: 8192 groups, Page 2: 6808 groups
        assert(total_rows == 15000);
        physical_operator_destroy(pipeline);
        remove(tmp_path);
    }

    // 2. Multi-page unaggregated SELECT + WHERE across Flushed Disk Granules + Active RAM Buffer (3 blocks)
    {
        // Flush the 15,000 in-memory rows to disk part ./all_1_1_0 (2 granules: 8192 + 6808 rows)
        assert(table_flush(table, ".") == 0);
        assert(table->num_parts == 1);
        assert(table->row_count == 0);

        // Insert 5,000 additional rows into active RAM buffer (user_id: 15000..19999)
        char tmp_path2[] = "/tmp/cdb_bulk2_XXXXXX";
        int fd2 = mkstemp(tmp_path2);
        assert(fd2 != -1);
        FILE *f2 = fdopen(fd2, "w");
        assert(f2 != NULL);
        for (int i = 15000; i < 20000; i++) {
            fprintf(f2, "{\"user_id\": %d, \"amount\": %d, \"region\": \"EU\"}\n", i, i % 5);
        }
        fclose(f2);

        char insert_q2[256];
        snprintf(insert_q2, sizeof(insert_q2), "INSERT INTO events FROM '%s';", tmp_path2);
        sql_parser_t parser = sql_parser_create(insert_q2);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        repl_exec_insert(table, &ast);
        assert(table->row_count == 5000);
        remove(tmp_path2);

        // Query without GROUP BY: streams 2 disk granule blocks (8192 + 6808) + 1 RAM block (5000) = 3 blocks
        const char *scan_filter_q = "SELECT user_id, amount FROM events WHERE amount < 100;";
        parser = sql_parser_create(scan_filter_q);
        assert(sql_parser_parse(&parser, &ast));
        physical_operator_t *pipeline = repl_exec_select(table, &ast);
        assert(pipeline != NULL);

        size_t total_rows = 0;
        size_t num_blocks = 0;
        while (true) {
            block_t block = exec_pipeline_run(pipeline);
            if (block.num_rows == 0)
                break;
            total_rows += block.num_rows;
            num_blocks++;
        }
        assert(num_blocks == 3); // Block 1: 8192 (disk), Block 2: 6808 (disk), Block 3: 5000 (RAM)
        assert(total_rows == 20000);
        physical_operator_destroy(pipeline);
    }

    // 3. Multi-page selective WHERE (amount > 0) + GROUP BY across Disk + RAM (2 blocks, 16,000 groups)
    {
        const char *agg_filter_q =
            "SELECT user_id, SUM(amount) FROM events WHERE amount > 0 GROUP BY user_id;";
        sql_parser_t parser = sql_parser_create(agg_filter_q);
        ast_query_t ast = {0};
        assert(sql_parser_parse(&parser, &ast));
        physical_operator_t *pipeline = repl_exec_select(table, &ast);
        assert(pipeline != NULL);

        size_t total_rows = 0;
        size_t num_blocks = 0;
        while (true) {
            block_t block = exec_pipeline_run(pipeline);
            if (block.num_rows == 0)
                break;
            for (size_t r = 0; r < block.num_rows; r++) {
                int32_t uid = block.columns[0].data[r];
                int32_t sum = block.columns[1].data[r];
                assert(sum > 0);
                assert(sum == (uid % 5));
            }
            total_rows += block.num_rows;
            num_blocks++;
        }
        // Out of 20,000 rows, 4/5 have amount > 0 -> 16,000 groups across 2 blocks (8192 + 7808)
        assert(num_blocks == 2);
        assert(total_rows == 16000);
        physical_operator_destroy(pipeline);

        remove("./all_1_1_0/user_id.bin");
        remove("./all_1_1_0/user_id.mrk");
        remove("./all_1_1_0/user_id.idx");
        remove("./all_1_1_0/amount.bin");
        remove("./all_1_1_0/amount.mrk");
        remove("./all_1_1_0/amount.idx");
        remove("./all_1_1_0/region.bin");
        remove("./all_1_1_0/region.mrk");
        remove("./all_1_1_0/region.idx");
        remove("./all_1_1_0/region.strings");
        remove("./all_1_1_0/region.offsets");
        remove("./all_1_1_0");
    }

    table_destroy(table);
    printf("Day 22 repl execution read-path runtime passed!\n");
}

int main(void) {
    database_t* db = db_open("test_db");
    assert(db != NULL);
    assert(db->name != NULL);
    db_close(db);

    test_allocator();
    test_slices();
    test_mmap_file();
    test_filter_and_gather();
    test_string_dict();
    test_bitmap();
    test_schema_registry();
    test_simd_primitives();
    test_column_writer();
    test_sparse_index_e2e();
    test_column_reader();
    test_raw_strings();
    test_tokenizer();
    test_inverted_index();
    test_table_creation();
    test_table_e2e_batch_ingestion();
    test_table_flush_e2e();
    test_deletion_vector();
    test_min_heap();
    test_external_sorter_in_memory();
    test_external_sorter_disk_spilling();
    test_grace_hash_agg();
    test_table_scanner();
    test_exec_pipeline();
    test_part_merger();
    test_sql_parser();
    test_repl_write_path_execution();
    test_repl_read_path_execution();

    printf("SUCCESS: All C11 unit tests passed!\n");
    return 0;
}
