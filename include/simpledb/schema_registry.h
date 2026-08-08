#pragma once

#include <stddef.h>
#include <stdint.h>

#define MAX_COLUMN_NAME_LEN 64

typedef enum { COL_TYPE_I32, COL_TYPE_F32, COL_TYPE_STRING_DICT, COL_TYPE_STRING } column_type_t;

typedef struct {
    char name[MAX_COLUMN_NAME_LEN];
    column_type_t type;
} schema_column_t;

typedef struct {
    schema_column_t *columns;
    size_t count;
    size_t capacity;
} schema_registry_t;

schema_registry_t *schema_registry_create(void);
void schema_registry_free(schema_registry_t *registry);

// Registers a new column and returns its index, or returns existing index if already present
int32_t schema_registry_add_column(schema_registry_t *registry, const char *name,
                                   column_type_t type);

// Returns the index of the column, or -1 if not found
int32_t schema_registry_get_column_index(const schema_registry_t *registry, const char *name);
