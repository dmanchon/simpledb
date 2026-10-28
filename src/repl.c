#include "simpledb/repl.h"
#include "simpledb/schema_registry.h"
#include "simpledb/sql_parser.h"
#include "simpledb/table.h"
#include "yyjson.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/_types/_ssize_t.h>

#define BULK_CHUNK_SIZE 1024
#define SCHEMA_SAMPLE_ROWS 10


static int32_t table_ensure_column(table_t *table, const char *name, column_type_t type) {
    if (!table->schema) {
        table->schema = schema_registry_create();
    }
    int32_t idx = schema_registry_get_column_index(table->schema, name);
    if (idx == -1) {
        idx = schema_registry_add_column(table->schema, name, type);
	size_t existing_rows = table->row_count;
        // Expand table->column_buffers to hold the new column buffer pointer
        table->column_buffers = realloc(table->column_buffers, sizeof(void *) * table->schema->count);
        if (type == COL_TYPE_I32) {
            buffer_i32_t *buf = buffer_i32_create(128);
	    for (size_t r = 0; r < existing_rows; r++) {
                buffer_i32_push(buf, 0);
            }
	    table->column_buffers[idx] = buf;
        } else {
	    raw_string_column_t *str_col = raw_string_column_create(existing_rows > 128 ? existing_rows * 2 : 128);
            // Pad existing in-memory rows with default ""
            for (size_t r = 0; r < existing_rows; r++) {
                str_col->strings[r] = strdup("");
            }
            str_col->size = existing_rows;
            table->column_buffers[idx] = str_col;
        }
    }
    return idx;
}

static void repl_exec_bulk_insert(table_t *db, const char *file_path) {
    FILE *fp = fopen(file_path, "rb");
    if (!fp) {
        printf("Error: failed to open file '%s'.\n", file_path);
	return;
    }
    // schema inference
    char *line = NULL;
    size_t cap = 0;
    for (int i = 0; i < SCHEMA_SAMPLE_ROWS; i++) {
	ssize_t len = getline(&line, &cap, fp);
	if (len == -1)
	    break; // Reached EOF before 100 rows

	// strip return
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
	    line[--len] = '\0';
	}
	if (len == 0)
	    continue;

	yyjson_doc *peek_doc = yyjson_read(line, (size_t)len, 0);
	if (peek_doc) {
	    yyjson_val *root = yyjson_doc_get_root(peek_doc);
	    if (yyjson_is_obj(root)) {
		size_t idx, max;
		yyjson_val *k, *v;
		yyjson_obj_foreach(root, idx, max, k, v) {
		    column_type_t t = (v && yyjson_is_int(v)) ? COL_TYPE_I32 : COL_TYPE_STRING;
		    table_ensure_column(db, yyjson_get_str(k), t);
		}
	    }
	    yyjson_doc_free(peek_doc);
	}
    }
    rewind(fp); // Reset file offset back to beginning
    

    if (!db->schema || db->schema->count == 0) {
        printf("Error: could not infer schema from '%s'.\n", file_path);
        fclose(fp);
        free(line);
        return;
    }

    // prepare data buffers
    size_t num_cols = db->schema->count;
    void **col_data = calloc(num_cols, sizeof(void *));
    const void **col_ptrs = calloc(num_cols, sizeof(const void *));

    for (size_t c = 0; c < num_cols; c++) {
        if (db->schema->columns[c].type == COL_TYPE_I32) {
            col_data[c] = malloc(sizeof(int32_t) * BULK_CHUNK_SIZE);
        } else {
            col_data[c] = malloc(sizeof(char *) * BULK_CHUNK_SIZE);
        }
    }

    size_t chunk_rows = 0;
    while (true) {
	ssize_t len = getline(&line, &cap, fp);
	if (len == -1)
	    break; // Reached EOF before 100 rows

	// strip return
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
	    line[--len] = '\0';
	}
	if (len == 0)
	    continue;

	yyjson_doc *peek_doc = yyjson_read(line, (size_t)len, 0);
	if (peek_doc) {
	    yyjson_val *root = yyjson_doc_get_root(peek_doc);
	    if (yyjson_is_obj(root)) {
		size_t idx, max;
                yyjson_val *k, *v;

		// reset buffers
                for (size_t c = 0; c < num_cols; c++) {
		    if (db->schema->columns[c].type == COL_TYPE_I32) {
			((int32_t *)col_data[c])[chunk_rows] = 0;
		    } else {
			((char **)col_data[c])[chunk_rows] = NULL;
		    }
                }
		
                yyjson_obj_foreach(root, idx, max, k, v) {
		      int32_t c = schema_registry_get_column_index(db->schema, yyjson_get_str(k));
                      if (c < 0)
                          continue;
		      if (db->schema->columns[c].type == COL_TYPE_I32) {
			  ((int32_t *)col_data[c])[chunk_rows] = yyjson_is_int(v) ? (int32_t)yyjson_get_int(v) : 0;
		      } else {
			  const char *s = yyjson_is_str(v) ? yyjson_get_str(v) : "";
			  ((char **)col_data[c])[chunk_rows] = strdup(s);
		      }
                }
		for (size_t c = 0; c < num_cols; c++) {
		    if (db->schema->columns[c].type != COL_TYPE_I32) {
			char **arr = (char **)col_data[c];
			if (!arr[chunk_rows]) arr[chunk_rows] = strdup("");
		    }
                }
		chunk_rows++;
	    }
	    yyjson_doc_free(peek_doc);
        }
	
	// flush
	if (chunk_rows == BULK_CHUNK_SIZE) {
	    table_insert_batch(db, (const void ***)col_data, num_cols, chunk_rows);
	    for (size_t c = 0; c < num_cols; c++) {
		if (db->schema->columns[c].type != COL_TYPE_I32) {
		    char **arr = (char **)col_data[c];
		    for (size_t r = 0; r < chunk_rows; r++) free(arr[r]);
		}
	    }
	    chunk_rows = 0;
	}
    }
    
    // incomplete last chunk
    if (chunk_rows > 0) {
	table_insert_batch(db, (const void ***)col_ptrs, num_cols, chunk_rows);
	for (size_t c = 0; c < num_cols; c++) {
	    if (db->schema->columns[c].type != COL_TYPE_I32) {
		char **arr = (char **)col_data[c];
		for (size_t r = 0; r < chunk_rows; r++) free(arr[r]);
	    }
	}
    }

    // cleanup
    free(line);
    fclose(fp);
    for (size_t c = 0; c < num_cols; c++) free(col_data[c]);
    free(col_data);
    free(col_ptrs);
}

void repl_exec_insert(table_t *db, ast_query_t *ast) {
    if (ast->is_bulk_insert) {
        repl_exec_bulk_insert(db, ast->from_file_path);
	return;
    }

    size_t max_cols = (db->schema ? db->schema->count : 0) + ast->num_insert_cols;
    if (max_cols == 0) return;

    
    int32_t *i32_vals = calloc(max_cols, sizeof(int32_t));
    const char **str_vals = calloc(max_cols, sizeof(char *));
    const void **col_ptrs = calloc(max_cols, sizeof(void *));
    for (size_t c = 0; c < max_cols; c++) {
        str_vals[c] = "";
    }

    for (size_t k = 0; k < ast->num_insert_cols; k++) {
        column_type_t type = ast->insert_vals[k].is_string ? COL_TYPE_STRING : COL_TYPE_I32;
        int32_t idx = table_ensure_column(db, ast->insert_cols[k], type);
        if (idx < 0) continue;
        if (type == COL_TYPE_I32) {
            i32_vals[idx] = ast->insert_vals[k].int_val;
        } else {
            str_vals[idx] = ast->insert_vals[k].string_val;
        }
    }

    for (size_t c = 0; c < db->schema->count; c++) {
        if (db->schema->columns[c].type == COL_TYPE_I32) {
            col_ptrs[c] = &i32_vals[c];
        } else {
            col_ptrs[c] = &str_vals[c];
        }
    }

    int rc = table_insert_batch(db, (const void ***)col_ptrs, db->schema->count, 1);
    if (rc == 0) {
        printf("INSERT 1 (table now has %zu rows in RAM).\n", db->row_count);
    } else {
        printf("Error: insert failed.\n");
    }
    
    free(i32_vals);
    free(str_vals);
    free(col_ptrs);
}

void repl_exec_select(table_t *db, ast_query_t *ast) {}


void repl_run(table_t *db) {
    if (!db)
        return;

    char *line = NULL;
    size_t cap = 0;

    while (true) {
	printf("db> ");
        fflush(stdout);

        ssize_t num = getline(&line, &cap, stdin);
        if (num == -1) break;

        while (num > 0 && (line[num - 1] == '\n' || line[num - 1] == '\r')) {
	    line[--num] = '\0';
        }

	if (num == 0)
	    continue;

	if (strcmp(line, ".exit") == 0 || strcmp(line, ".quit") == 0) {
            break;
        }

        if (strcmp(line, ".flush") == 0) {
            table_flush(db, ".");
            printf("OK (flushed to disk part).\n");
            continue;
        }

        sql_parser_t parser = sql_parser_create(line);
	ast_query_t ast = {0};
        if (!sql_parser_parse(&parser, &ast)) {
            printf("Error: invalid SQL syntax.\n");
            continue;
        }
        if (ast.stmt_type == STMT_INSERT) {
            repl_exec_insert(db, &ast);
        } else if (ast.stmt_type == STMT_SELECT) {
            repl_exec_select(db, &ast);
        }
    }

    free(line);
}
