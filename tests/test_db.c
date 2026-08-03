#include "simpledb/db.h"
#include "simpledb/allocator.h"
#include "simpledb/slice.h"
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

int main(void) {
    database_t* db = db_open("test_db");
    assert(db != NULL);
    assert(db->name != NULL);
    db_close(db);

    test_allocator();
    test_slices();

    printf("SUCCESS: All C11 unit tests passed!\n");
    return 0;
}
