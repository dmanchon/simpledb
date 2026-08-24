// include/simpledb/fts.h

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "simpledb/slice.h"
#include "simpledb/bitmap.h"
#include "simpledb/string_dict.h"

#define INITIAL_CAPACITY 25000

typedef struct {
    bitmap_t** postings;   // Array of pointers to bitmaps (1 bitmap per term)
    string_dict_t* map;
    size_t num_terms;
    size_t capacity;
} inverted_index_t;

inverted_index_t* inverted_index_create(void);

void inverted_index_add_document(inverted_index_t* idx, const char* text, size_t granule_id);

bitmap_t* inverted_index_search(inverted_index_t* idx, const char* term);

void inverted_index_free(inverted_index_t* idx);
