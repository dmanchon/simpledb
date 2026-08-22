#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Token types for the lexer
typedef enum {
    TOKEN_EOF = 0,
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_GROUP,
    TOKEN_ORDER,
    TOKEN_BY,
    TOKEN_ASC,
    TOKEN_DESC,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_COMMA,
    TOKEN_EQUALS,
    TOKEN_GREATER,
    TOKEN_LESS,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_ERROR
} token_type_t;

// A single token
typedef struct {
    token_type_t type;
    char text[64];
    int length;
} sql_token_t;

// The Lexer state
typedef struct {
    const char* input;
    size_t pos;
    size_t len;
} sql_lexer_t;

sql_lexer_t sql_lexer_create(const char* query);
sql_token_t sql_lexer_next_token(sql_lexer_t* lexer);

// Abstract Syntax Tree (AST) Nodes
typedef enum {
    AST_AGG_SUM,
    AST_AGG_MAX,
    AST_AGG_MIN,
    AST_AGG_COUNT,
    AST_PROJECTION
} ast_expr_type_t;

typedef struct {
    ast_expr_type_t type;
    char column_name[64];
    char alias[64];
} ast_expr_t;

typedef enum {
    OP_EQUALS,
    OP_GREATER_THAN,
    OP_LESS_THAN
} ast_op_t;

typedef struct {
    char column_name[64];
    ast_op_t op;
    char string_val[64];
    int32_t int_val;
    bool is_string;
} ast_predicate_t;

typedef enum {
    STMT_SELECT,
    STMT_INSERT
} ast_stmt_type_t;

typedef struct {
    int32_t int_val;
    char string_val[64];
    bool is_string;
} ast_value_t;

typedef struct {
    ast_stmt_type_t stmt_type;
    char table_name[64];
    
    // For SELECT
    ast_expr_t select_exprs[16];
    size_t num_select_exprs;
    ast_predicate_t predicates[16];
    size_t num_predicates;
    char group_by_col[64];
    bool has_group_by;
    char order_by_col[64];
    bool has_order_by;
    bool order_desc;

    // For INSERT (e.g., INSERT INTO table (col1, col2) VALUES (1, 'a'))
    char insert_cols[16][64];
    size_t num_insert_cols;
    ast_value_t insert_vals[16];
    size_t num_insert_vals;
    
    // For bulk JSON insertion (e.g., INSERT INTO table FROM 'data.json')
    bool is_bulk_insert;
    char from_file_path[256];
} ast_query_t;

// The Parser state
typedef struct {
    sql_lexer_t lexer;
    sql_token_t current_token;
    sql_token_t peek_token;
} sql_parser_t;

sql_parser_t sql_parser_create(const char* query);
bool sql_parser_parse(sql_parser_t* parser, ast_query_t* out_query);

