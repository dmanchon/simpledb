#pragma once

#include "simpledb/table.h"
#include "simpledb/sql_parser.h"
#include "simpledb/exec_pipeline.h"

void repl_run(table_t *db);
void repl_exec_insert(table_t *db, ast_query_t *ast);
physical_operator_t *repl_exec_select(table_t *db, ast_query_t *ast);
