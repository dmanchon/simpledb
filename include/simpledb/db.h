#pragma once

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    const char* name;
} database_t;

database_t* db_open(const char* name);
void db_close(database_t* db);
