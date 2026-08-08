#include "simpledb/db.h"
#include "simpledb/allocator.h"
#include "simpledb/slice.h"
#include "simpledb/mmap_file.h"
#include "simpledb/filter.h"
#include "simpledb/string_dict.h"
#include <assert.h>
#include <stdint.h>
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

    // Cleanup
    buffer_i32_free(salary_buf);
    selection_vector_free(sel);
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

    printf("SUCCESS: All C11 unit tests passed (Day 1 - Day 6)!\n");
    return 0;
}
