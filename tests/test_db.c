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
#include "simpledb/tokenizer.h"
#include "simpledb/deletion_vector.h"
#include <time.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

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

    printf("SUCCESS: All C11 unit tests passed!\n");
    return 0;
}
