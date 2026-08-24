#include "simpledb/tokenizer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

tokenizer_t tokenizer_create(const char *input, size_t input_len) {
    tokenizer_t tokenizer = {.input = input, .input_len = input_len, .current_pos = 0};
    return tokenizer;
}

static bool is_token_char(char c) {
    unsigned char uc = (unsigned char)c;

    if (isalnum(uc))
        return true;

    if (uc == '\'' || uc == '-')
        return true;

    if (uc >= 128)
        return true;

    return false;
}

int tokenizer_next(tokenizer_t *tokenizer, token_t *out_token) {
    if (!tokenizer || !out_token || !tokenizer->input)
        return 0;

    while (tokenizer->current_pos < tokenizer->input_len &&
           !is_token_char(tokenizer->input[tokenizer->current_pos])) {
        tokenizer->current_pos++;
    }

    if (tokenizer->current_pos >= tokenizer->input_len) {
        return 0;
    }

    size_t start_pos = tokenizer->current_pos;

    while (tokenizer->current_pos < tokenizer->input_len &&
           is_token_char(tokenizer->input[tokenizer->current_pos])) {
        tokenizer->current_pos++;
    }

    out_token->text = tokenizer->input + start_pos;
    out_token->len = tokenizer->current_pos - start_pos;

    return 1;
}
