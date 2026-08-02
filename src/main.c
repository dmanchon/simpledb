#include "simpledb/db.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("simpledb version 0.1.0 (C11 Edition)\n");
        return 0;
    }

    printf("Starting simpledb C11 Columnar Database Engine...\n");
    database_t* db = db_open("default");
    if (db) {
        printf("Database opened successfully: %s\n", db->name);
        db_close(db);
    }
    return 0;
}
