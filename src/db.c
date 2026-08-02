#include "simpledb/db.h"
#include <stdlib.h>
#include <stdio.h>

database_t* db_open(const char* name) {
    database_t* db = (database_t*)malloc(sizeof(database_t));
    if (!db) return NULL;
    db->name = name;
    return db;
}

void db_close(database_t* db) {
    if (db) {
        free(db);
    }
}
