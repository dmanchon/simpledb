#ifndef SIMPLEDB_TOKENIZER_H
#define SIMPLEDB_TOKENIZER_H

#include <stddef.h>

// A zero-copy slice of a string representing a single word
typedef struct {
    const char* text;
    size_t len;
} token_t;

// The stateful iterator for the Tokenizer
typedef struct {
    const char* input;
    size_t input_len;
    size_t current_pos;
} tokenizer_t;

// Initialize the tokenizer with a raw string
tokenizer_t tokenizer_create(const char* input, size_t input_len);

// Yield the next token (returns 1 if found, 0 if EOF)
int tokenizer_next(tokenizer_t* tokenizer, token_t* out_token);

#endif
