#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "simpledb/allocator.h"

#define STRING_DICT_MAX_ENTRIES 65535
#define STRING_DICT_NOT_FOUND   (-1)
#define STRING_DICT_HASH_SLOTS  131072  // Power of 2, >= 2 * MAX_ENTRIES

typedef struct {
    char**    strings;      // Heap-allocated string pointers, one per unique entry
    uint32_t* hash_table;   // Open-addressing hash index: slot -> entry index + 1 (0 = empty)
    int32_t   count;        // Number of unique strings inserted so far
} string_dict_t;

string_dict_t* string_dict_create(void);
void string_dict_free(string_dict_t* dict);

int32_t string_dict_insert(string_dict_t* dict, const char* str);
int32_t string_dict_lookup(const string_dict_t* dict, const char* str);

bool string_dict_encode_column(string_dict_t* dict,
                               const char**   raw_strings,
                               size_t         count,
                               buffer_i32_t*  out_keys);
