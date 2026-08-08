#include "simpledb/string_dict.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

string_dict_t* string_dict_create(void) {
    string_dict_t* dict = (string_dict_t*)malloc(sizeof(string_dict_t));
    if (!dict) {
        fprintf(stderr, "[FATAL ERROR] failed to allocate string_dict_t\n");
        exit(EXIT_FAILURE);
    }

    dict->count = 0;
    dict->strings = calloc(STRING_DICT_MAX_ENTRIES, sizeof(char*));
    dict->hash_table = calloc(STRING_DICT_HASH_SLOTS, sizeof(uint32_t));

    if (!dict->strings || !dict->hash_table) {
        fprintf(stderr, "[FATAL ERROR] failed to allocate string_dict internal arrays\n");
        exit(EXIT_FAILURE);
    }

    return dict;
}

void string_dict_free(string_dict_t *dict) {
    if (!dict) return;

    for (int32_t i = 0; i < dict->count; i++) {
        free(dict->strings[i]);
    }
    free(dict->strings);
    free(dict->hash_table);
    free(dict);
}


static uint32_t string_dict_hash(const char* str) {
    // FNV-1a 32-bit hash
    uint32_t h = 2166136261u;
    while (*str) {
        h ^= (uint8_t)(*str++);
        h *= 16777619u;
    }
    return h;
}

int32_t string_dict_insert(string_dict_t* dict, const char* str) {
    if (!dict || !str) return STRING_DICT_NOT_FOUND;
    if (dict->count >= STRING_DICT_MAX_ENTRIES) return STRING_DICT_NOT_FOUND;

    uint32_t h = string_dict_hash(str);
    // Fast Modulo: h % STRING_DICT_HASH_SLOTS
    uint32_t slot = h & (STRING_DICT_HASH_SLOTS - 1);

    while (dict->hash_table[slot] != 0) {
        int32_t idx = (int32_t)(dict->hash_table[slot] - 1);
        if (strcmp(dict->strings[idx], str) == 0) {
            return idx;  // Existing entry — return its key
        }
        slot = (slot + 1) & (STRING_DICT_HASH_SLOTS - 1);
    }

    int32_t new_key = dict->count;
    dict->strings[new_key] = strdup(str);
    dict->hash_table[slot] = (uint32_t)(new_key + 1);
    dict->count++;

    return new_key;
}

int32_t string_dict_lookup(const string_dict_t* dict, const char* str) {
    if (!dict || !str) return STRING_DICT_NOT_FOUND;

    uint32_t h = string_dict_hash(str);
    // Fast Modulo: h % STRING_DICT_HASH_SLOTS
    uint32_t slot = h & (STRING_DICT_HASH_SLOTS - 1);

    while (dict->hash_table[slot] != 0) {
        int32_t idx = (int32_t)(dict->hash_table[slot] - 1);
        if (strcmp(dict->strings[idx], str) == 0) {
            return idx;
        }
        slot = (slot + 1) & (STRING_DICT_HASH_SLOTS - 1);
    }
    return STRING_DICT_NOT_FOUND;
}

bool string_dict_encode_column(string_dict_t* dict,
                               const char**   raw_strings,
                               size_t         count,
                               buffer_i32_t*  out_keys) {
    if (!dict || !raw_strings || !out_keys) return false;

    buffer_i32_ensure_capacity(out_keys, out_keys->size + count);

    for (size_t i = 0; i < count; i++) {
        int32_t key = string_dict_insert(dict, raw_strings[i]);
        if (key == STRING_DICT_NOT_FOUND) return false;
        buffer_i32_push(out_keys, key);
    }
    return true;
}
