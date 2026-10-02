#include "simpledb/part_merger.h"
#include "simpledb/table.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main() {
    // 1. Setup a Table
    schema_registry_t* schema = schema_registry_create();
    schema_registry_add_column(schema, "user_id", COL_TYPE_I32);
    schema_registry_add_column(schema, "amount", COL_TYPE_I32);
    
    table_t* table = table_create("events_merge_test", schema);
    
    // 2. Insert Data and Flush to create Part 1 (simulate batch 1)
    int32_t users1[] = {1, 2};
    int32_t amounts1[] = {100, 200};
    buffer_i32_t b_u1 = { .data = users1, .size = 2, .capacity = 2 };
    buffer_i32_t b_a1 = { .data = amounts1, .size = 2, .capacity = 2 };
    
    table_insert_column_i32(table, "user_id", &b_u1);
    table_insert_column_i32(table, "amount", &b_a1);
    table_flush(table, "./data_dir"); // Creates part 0
    
    // 3. Insert Data and Flush to create Part 2 (simulate batch 2)
    int32_t users2[] = {3, 4};
    int32_t amounts2[] = {300, 400};
    buffer_i32_t b_u2 = { .data = users2, .size = 2, .capacity = 2 };
    buffer_i32_t b_a2 = { .data = amounts2, .size = 2, .capacity = 2 };
    
    table_insert_column_i32(table, "user_id", &b_u2);
    table_insert_column_i32(table, "amount", &b_a2);
    table_flush(table, "./data_dir"); // Creates part 1
    
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
    size_t new_part_id = part_merger_execute(merger);
    part_merger_destroy(merger);
    
    assert(new_part_id != (size_t)-1);
    
    // 5. Verify the Table State
    // The table should now only have 1 active, consolidated part!
    assert(table->num_parts == 1);
    
    // 6. Cleanup
    table_destroy(table);
    schema_registry_destroy(schema);
    
    printf("Day 19 MergeTree Part Compactor test passed!\n");
    return 0;
}
