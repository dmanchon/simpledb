#include "simpledb/fts.h"
#include "simpledb/bitmap.h"
#include "simpledb/string_dict.h"
#include "simpledb/tokenizer.h"
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

inverted_index_t *inverted_index_create(void) {
    inverted_index_t *index = malloc(sizeof(inverted_index_t));

    index->capacity = INITIAL_CAPACITY;
    index->map = string_dict_create();
    index->num_terms = 0;
    index->postings = calloc(sizeof(*(index->postings)), INITIAL_CAPACITY);

    return index;
}

void inverted_index_add_document(inverted_index_t *idx, const char *text, size_t granule_id) {
    tokenizer_t tokenizer = tokenizer_create(text, strlen(text));
    token_t token;
    while (tokenizer_next(&tokenizer, &token)) {
        // discard long terms
        if (token.len >= 128)
            continue;

        // copy and lowercase
        char term[128];
        memcpy(term, token.text, token.len);
        term[token.len] = '\0';

        for (size_t i = 0; i < token.len; i++) {
            term[i] = tolower((unsigned char)term[i]);
        }

        // look for it in the dict
        int32_t slot = string_dict_insert(idx->map, term);
        if (idx->postings[slot] == NULL) {
            // not there yet, create the bitmap
            bitmap_t *bitmap = bitmap_create(128);
            bitmap_set(bitmap, granule_id);
            idx->postings[slot] = bitmap;
            idx->num_terms++;
        } else {
            bitmap_set(idx->postings[slot], granule_id);
        }
    }
}

bitmap_t *inverted_index_search(inverted_index_t *idx, const char *term) {
    size_t len = strlen(term);
    if (len >= 128)
        return NULL;

    // copy and lowercase
    char term_lower[128];
    memcpy(term_lower, term, len);
    term_lower[len] = '\0';

    for (size_t i = 0; i < len; i++) {
        term_lower[i] = tolower((unsigned char)term_lower[i]);
    }

    int32_t slot = string_dict_lookup(idx->map, term_lower);
    if (slot == STRING_DICT_NOT_FOUND) {
        return NULL;
    }

    return idx->postings[slot];
}

void inverted_index_free(inverted_index_t *idx) {
    string_dict_free(idx->map);
    for (size_t i=0; i < idx->num_terms; i++) {
        bitmap_free(idx->postings[i]);
    }
    free(idx->postings);
    free(idx);
}
