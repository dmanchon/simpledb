#include "simpledb/schema_registry.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_SCHEMA_CAPACITY 100

schema_registry_t *schema_registry_create(void) {
    schema_registry_t *registry = malloc(sizeof(schema_registry_t));
    if (!registry) {
        fprintf(stderr, "[FATAL ERROR] OOM cannot create schema_registry\n");
        exit(EXIT_FAILURE);
    }

    registry->columns = calloc(INITIAL_SCHEMA_CAPACITY, sizeof(schema_column_t));
    if (!registry->columns) {
        fprintf(stderr, "[FATAL ERROR] OOM cannot allocate schema_columns array\n");
        exit(EXIT_FAILURE);
    }

    registry->count = 0;
    registry->capacity = INITIAL_SCHEMA_CAPACITY;

    return registry;
}

void schema_registry_free(schema_registry_t *registry) {
    if (!registry)
        return;
        
    free(registry->columns);
    free(registry);
}

int32_t schema_registry_add_column(schema_registry_t *registry, const char *name,
                                   column_type_t type) {
    if(!registry || !name) return -1;

    int32_t index = schema_registry_get_column_index(registry, name);
    if(index != -1) {
        if(registry->columns[index].type != type) {
            fprintf(stderr, "[FATAL ERROR] Schema mismatch for column '%s'\n", name);
            exit(EXIT_FAILURE);
        }
        return index;
    }

    if (registry->count >= registry->capacity) {
        // resize the array
        registry->capacity *= 2;
        registry->columns =
            realloc(registry->columns, registry->capacity * sizeof(schema_column_t));
        if (!registry->columns) {
            fprintf(stderr, "[FATAL ERROR] OOM cannot allocate schema_columns array\n");
            exit(EXIT_FAILURE);
        }
    }
    
    size_t new_index = registry->count;
    strncpy(registry->columns[new_index].name, name, MAX_COLUMN_NAME_LEN - 1);
    registry->columns[new_index].name[MAX_COLUMN_NAME_LEN - 1] = '\0';
    registry->columns[new_index].type = type;
    
    registry->count++;
    return (int32_t)new_index;
}

int32_t schema_registry_get_column_index(const schema_registry_t *registry, const char *name) {
    if (!registry || !name)
        return -1;

    for (size_t i = 0; i < registry->count; i++) {
        if (strcmp(registry->columns[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}
