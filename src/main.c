#include "simpledb/repl.h"
#include "simpledb/table.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("simpledb version 0.1.0 (C11 Edition)\n");
        return 0;
    }

    const char *table_name = (argc > 1) ? argv[1] : "events";
    printf("Starting simpledb C11 Columnar Database Engine (table: %s)...\n", table_name);

    table_t *table = table_create(table_name);
    if (table) {
        repl_run(table);
        table_destroy(table);
    }
    return 0;
}
