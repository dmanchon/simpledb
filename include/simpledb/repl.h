#pragma once

#include "simpledb/table.h"
#include <stdbool.h>

typedef struct {
    table_t* current_db; // For now, the REPL only holds one table open
    bool running;
} repl_t;

repl_t repl_create(table_t* initial_db);
void repl_run(repl_t* repl);
void repl_destroy(repl_t* repl);

