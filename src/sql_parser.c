#include "simpledb/sql_parser.h"
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

sql_lexer_t sql_lexer_create(const char *query) {
    if (!query) {
        return (sql_lexer_t){.input = "", .len = 0, .pos = 0};
    }
    size_t len = strlen(query);
    sql_lexer_t lexer = (sql_lexer_t){.input = query, .len = len, .pos = 0};
    return lexer;
}

sql_token_t sql_lexer_next_token(sql_lexer_t *lexer) {

    while (lexer->pos < lexer->len && isspace((unsigned char)lexer->input[lexer->pos])) {
        lexer->pos++;
    }

    if (lexer->pos >= lexer->len) {
        return (sql_token_t){.type = TOKEN_EOF, .text = "", .length = 0};
    }
    char c = lexer->input[lexer->pos];

    switch (c) {
    case ',':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_COMMA, .text = ",", .length = 1};
    case '(':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_LPAREN, .text = "(", .length = 1};
    case ')':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_RPAREN, .text = ")", .length = 1};
    case '=':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_EQUALS, .text = "=", .length = 1};
    case '>':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_GREATER, .text = ">", .length = 1};
    case '<':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_LESS, .text = "<", .length = 1};
    case ';':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_EOF, .text = ";", .length = 1};
    }

    // quoted text
    if (c == '\'') {
        lexer->pos++; // skip opening quote
        size_t start = lexer->pos;
        while (lexer->pos < lexer->len && lexer->input[lexer->pos] != '\'') {
            lexer->pos++;
        }
        size_t len = lexer->pos - start;
        sql_token_t tok = {.type = TOKEN_STRING, .length = (int)len};
        if (len >= sizeof(tok.text)) {
            return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
        }
        memcpy(tok.text, &lexer->input[start], len);
        tok.text[len] = '\0';
        if (lexer->pos < lexer->len && lexer->input[lexer->pos] == '\'') {
            lexer->pos++; // skip closing quote
        }
        return tok;
    }

    // numbers
    if (isdigit((unsigned char)c)) {
        size_t start = lexer->pos;
        do {
            lexer->pos++;
        } while (lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos]));

        // float
        if (lexer->pos + 1 < lexer->len && lexer->input[lexer->pos] == '.' &&
            isdigit((unsigned char)lexer->input[lexer->pos + 1])) {
            do {
                lexer->pos++;
            } while (lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos]));
        }

        size_t len = lexer->pos - start;
        sql_token_t tok = {.type = TOKEN_NUMBER, .length = (int)len};

        if (len >= sizeof(tok.text)) {
            return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
        }
        memcpy(tok.text, &lexer->input[start], len);
        tok.text[len] = '\0';
        return tok;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lexer->pos;
        do {
            lexer->pos++;
        } while (lexer->pos < lexer->len &&
                 (isalnum((unsigned char)lexer->input[lexer->pos]) ||
                  lexer->input[lexer->pos] == '_' || lexer->input[lexer->pos] == '.'));
        size_t len = lexer->pos - start;
        sql_token_t tok = {.type = TOKEN_IDENTIFIER, .length = (int)len};
        if (len >= sizeof(tok.text)) {
            return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
        }
        memcpy(tok.text, &lexer->input[start], len);
        tok.text[len] = '\0';
        if (strcasecmp(tok.text, "SELECT") == 0)
            tok.type = TOKEN_SELECT;
        else if (strcasecmp(tok.text, "FROM") == 0)
            tok.type = TOKEN_FROM;
        else if (strcasecmp(tok.text, "WHERE") == 0)
            tok.type = TOKEN_WHERE;
        else if (strcasecmp(tok.text, "GROUP") == 0)
            tok.type = TOKEN_GROUP;
        else if (strcasecmp(tok.text, "BY") == 0)
            tok.type = TOKEN_BY;
        else if (strcasecmp(tok.text, "INSERT") == 0)
            tok.type = TOKEN_INSERT;
        else if (strcasecmp(tok.text, "INTO") == 0)
            tok.type = TOKEN_INTO;
        else if (strcasecmp(tok.text, "VALUES") == 0)
            tok.type = TOKEN_VALUES;
        else if (strcasecmp(tok.text, "ORDER") == 0)
            tok.type = TOKEN_ORDER;
        else if (strcasecmp(tok.text, "ASC") == 0)
            tok.type = TOKEN_ASC;
        else if (strcasecmp(tok.text, "DESC") == 0)
            tok.type = TOKEN_DESC;
        return tok;
    }

    lexer->pos++;
    return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
}

sql_parser_t sql_parser_create(const char *query) {
    sql_lexer_t lexer = sql_lexer_create(query);
    sql_token_t current = sql_lexer_next_token(&lexer);
    sql_token_t peek = sql_lexer_next_token(&lexer);
    return (sql_parser_t){.lexer = lexer, .current_token = current, .peek_token = peek};
}

static void parser_advance(sql_parser_t *parser) {
    parser->current_token = parser->peek_token;
    parser->peek_token = sql_lexer_next_token(&parser->lexer);
}

static bool sql_parser_parse_insert(sql_parser_t *parser, ast_query_t *out_query) {
    out_query->stmt_type = STMT_INSERT;
    parser_advance(parser); // consume INSERT

    if (parser->current_token.type != TOKEN_INTO)
        return false;
    parser_advance(parser); // consume INTO

    if (parser->current_token.type != TOKEN_IDENTIFIER)
        return false;
    memcpy(out_query->table_name, parser->current_token.text, parser->current_token.length);
    parser_advance(parser); // consume table name

    // Branch A: Bulk JSON (INSERT INTO <table> FROM '<path>')
    if (parser->current_token.type == TOKEN_FROM) {
        parser_advance(parser); // consume FROM
        if (parser->current_token.type != TOKEN_STRING)
            return false;
        out_query->is_bulk_insert = true;
        memcpy(out_query->from_file_path, parser->current_token.text, parser->current_token.length);
        parser_advance(parser); // consume file path
        return true;
    }

    // Branch B: Row Values (INSERT INTO <table> (col1, col2) VALUES (val1, val2))
    if (parser->current_token.type != TOKEN_LPAREN)
        return false;
    parser_advance(parser); // consume '('

    while (parser->current_token.type == TOKEN_IDENTIFIER) {
        memcpy(out_query->insert_cols[out_query->num_insert_cols++], parser->current_token.text,
               parser->current_token.length);
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_COMMA)
            parser_advance(parser);
    }
    if (parser->current_token.type != TOKEN_RPAREN)
        return false;
    parser_advance(parser); // consume ')'

    if (parser->current_token.type != TOKEN_VALUES)
        return false;
    parser_advance(parser); // consume VALUES
    if (parser->current_token.type != TOKEN_LPAREN)
        return false;
    parser_advance(parser); // consume '('

    while (parser->current_token.type == TOKEN_NUMBER ||
           parser->current_token.type == TOKEN_STRING) {
        ast_value_t *val = &out_query->insert_vals[out_query->num_insert_vals++];
        if (parser->current_token.type == TOKEN_NUMBER) {
            val->is_string = false;
            val->int_val = atoi(parser->current_token.text);
        } else {
            val->is_string = true;
            memcpy(val->string_val, parser->current_token.text, parser->current_token.length);
        }
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_COMMA)
            parser_advance(parser);
    }
    if (parser->current_token.type != TOKEN_RPAREN)
        return false;
    parser_advance(parser); // consume ')'

    return out_query->num_insert_cols == out_query->num_insert_vals;
}

static bool sql_parser_parse_select(sql_parser_t *parser, ast_query_t *out_query) {

    out_query->stmt_type = STMT_SELECT;
    parser_advance(parser);

    // SELET ... expressions
    size_t n = 0;
    while (parser->current_token.type == TOKEN_IDENTIFIER) {
        ast_expr_t expr = (ast_expr_t){0};
        if (parser->peek_token.type == TOKEN_LPAREN) {
            if (strcasecmp(parser->current_token.text, "SUM") == 0)
                expr.type = AST_AGG_SUM;
            else if (strcasecmp(parser->current_token.text, "COUNT") == 0)
                expr.type = AST_AGG_COUNT;
            else if (strcasecmp(parser->current_token.text, "MAX") == 0)
                expr.type = AST_AGG_MAX;
            else if (strcasecmp(parser->current_token.text, "MIN") == 0)
                expr.type = AST_AGG_MIN;
            else
                return false;
            parser_advance(parser); // consume function name (lands on '(')
            parser_advance(parser); // consume '(' (lands on column name)
            if (parser->current_token.type != TOKEN_IDENTIFIER)
                return false;
            memcpy(expr.column_name, parser->current_token.text, parser->current_token.length);
            parser_advance(parser); // consume column name (lands on ')')
            if (parser->current_token.type != TOKEN_RPAREN)
                return false;
            parser_advance(parser); // consume ')'
        } else {
            expr.type = AST_PROJECTION;
            memcpy(expr.column_name, parser->current_token.text, parser->current_token.length);
            parser_advance(parser);
        }
        if (parser->peek_token.type == TOKEN_IDENTIFIER &&
            strcasecmp(parser->current_token.text, "AS") == 0) {
            parser_advance(parser);
            memcpy(expr.alias, parser->current_token.text, parser->current_token.length);
            parser_advance(parser);
        }
        out_query->select_exprs[n++] = expr;
        if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
        }
    }
    out_query->num_select_exprs = n;

    // FROM 'table'
    if (parser->current_token.type != TOKEN_FROM)
        return false;
    parser_advance(parser); // consume FROM
    if (parser->current_token.type != TOKEN_IDENTIFIER)
        return false;
    memcpy(out_query->table_name, parser->current_token.text, parser->current_token.length);
    parser_advance(parser); // consume table name

    // WHERE ... cond
    if (parser->current_token.type == TOKEN_WHERE) {
        parser_advance(parser); // consume WHERE
        if (parser->current_token.type != TOKEN_IDENTIFIER)
            return false;
        ast_predicate_t *pred = &out_query->predicates[out_query->num_predicates++];
        memcpy(pred->column_name, parser->current_token.text, parser->current_token.length);
        parser_advance(parser); // consume column name
        if (parser->current_token.type == TOKEN_EQUALS)
            pred->op = OP_EQUALS;
        else if (parser->current_token.type == TOKEN_GREATER)
            pred->op = OP_GREATER_THAN;
        else if (parser->current_token.type == TOKEN_LESS)
            pred->op = OP_LESS_THAN;
        else
            return false;
        parser_advance(parser); // consume operator
        if (parser->current_token.type == TOKEN_NUMBER) {
            pred->is_string = false;
            pred->int_val = atoi(parser->current_token.text);
        } else if (parser->current_token.type == TOKEN_STRING) {
            pred->is_string = true;
            memcpy(pred->string_val, parser->current_token.text, parser->current_token.length);
        } else {
            return false;
        }
        parser_advance(parser); // consume value
    }

    // GROUP BY
    if (parser->current_token.type == TOKEN_GROUP) {
        parser_advance(parser); // consume GROUP
        if (parser->current_token.type != TOKEN_BY)
            return false;
        parser_advance(parser); // consume BY
        if (parser->current_token.type != TOKEN_IDENTIFIER)
            return false;
        memcpy(out_query->group_by_col, parser->current_token.text, parser->current_token.length);
        out_query->has_group_by = true;
        parser_advance(parser); // consume column name
    }

    // order by ...
    if (parser->current_token.type == TOKEN_ORDER) {
        parser_advance(parser); // consume ORDER
        if (parser->current_token.type != TOKEN_BY)
            return false;
        parser_advance(parser); // consume BY
        if (parser->current_token.type != TOKEN_IDENTIFIER)
            return false;
        memcpy(out_query->order_by_col, parser->current_token.text, parser->current_token.length);
        out_query->has_order_by = true;
        out_query->order_desc = false; // default ASC
        parser_advance(parser);        // consume column name
        if (parser->current_token.type == TOKEN_DESC) {
            out_query->order_desc = true;
            parser_advance(parser);
        } else if (parser->current_token.type == TOKEN_ASC) {
            parser_advance(parser);
        }
    }
    return true;
}

bool sql_parser_parse(sql_parser_t *parser, ast_query_t *out_query) {
    if (!parser || !out_query)
        return false;
    *out_query = (ast_query_t){0};

    if (parser->current_token.type == TOKEN_INSERT) {
        return sql_parser_parse_insert(parser, out_query);
    } else if (parser->current_token.type == TOKEN_SELECT) {
        return sql_parser_parse_select(parser, out_query);
    } else {
        return false;
    }
}
