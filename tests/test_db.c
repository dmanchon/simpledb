#include "simpledb/db.h"
#include "simpledb/allocator.h"
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

int main(void) {
    database_t* db = db_open("test_db");
    assert(db != NULL);
    assert(db->name != NULL);
    db_close(db);

    test_allocator();

    printf("SUCCESS: All C11 unit tests passed!\n");
    return 0;
}
