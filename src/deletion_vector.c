#include "simpledb/deletion_vector.h"
#include "simpledb/bitmap.h"
#include "simpledb/storage.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

deletion_vector_t *deletion_vector_create(uint32_t initial_capacity) {
    deletion_vector_t *dv = malloc(sizeof(*dv));
    if (!dv) {
        fprintf(
            stderr,
            "[FATAL ERROR] deletion_vector_create failed to allocate deletion_vector_t struct\n");
        exit(EXIT_FAILURE);
    }
    dv->capacity = initial_capacity;
    dv->num_containers = 0;
    dv->deleted_count = 0;
    dv->num_rows = 0;

    dv->containers = malloc(sizeof(*dv->containers) * initial_capacity);
    if (!dv->containers) {
        fprintf(stderr, "[FATAL ERROR] containers failed to allocate roaring_container_t struct\n");
        exit(EXIT_FAILURE);
    }

    dv->keys = malloc(sizeof(*dv->keys) * initial_capacity);
    if (!dv->keys) {
        fprintf(stderr, "[FATAL ERROR] keys failed to allocate int16_t struct\n");
        exit(EXIT_FAILURE);
    }

    return dv;
}

void deletion_vector_add(deletion_vector_t *dv, uint32_t row_id) {
    uint16_t high_key = row_id >> 16;
    uint16_t low_key = row_id & 0xFFFF;

    // find the container
    roaring_container_t *container = NULL;
    size_t insert_idx = 0;
    for (; insert_idx < dv->num_containers; insert_idx++) {
        if (dv->keys[insert_idx] == high_key) {
            container = dv->containers[insert_idx];
            break;
        } else if (dv->keys[insert_idx] > high_key)
            break;
    }

    // or add a new one
    if (!container) {
        // check if we need to reallocate
        if (dv->num_containers >= dv->capacity) {
            // double it!
            dv->capacity *= 2;
            void *temp;
            temp = realloc(dv->containers, sizeof(*dv->containers) * dv->capacity);
            if (!temp) {
                fprintf(stderr,
                        "[FATAL ERROR] containers failed to allocate roaring_container_t struct\n");
                exit(EXIT_FAILURE);
            }
            dv->containers = temp;

            temp = realloc(dv->keys, sizeof(*dv->keys) * dv->capacity);
            if (!temp) {
                fprintf(stderr, "[FATAL ERROR] keys failed to allocate int16_t struct\n");
                exit(EXIT_FAILURE);
            }
            dv->keys = temp;
        }

        // now we insert the container
        size_t n = dv->num_containers - insert_idx;
        memmove(&dv->keys[insert_idx + 1], &dv->keys[insert_idx], n * sizeof(*dv->keys));
        memmove(&dv->containers[insert_idx + 1], &dv->containers[insert_idx],
                n * sizeof(*dv->containers));

        dv->keys[insert_idx] = high_key;
        dv->containers[insert_idx] = malloc(sizeof(roaring_container_t));

        if (!dv->containers[insert_idx]) {
            fprintf(stderr,
                    "[FATAL ERROR] container failed to allocate roaring_container_t struct\n");
            exit(EXIT_FAILURE);
        }

        const uint32_t capacity = 16;
        dv->num_containers++;
        dv->containers[insert_idx]->cardinality = 0;
        dv->containers[insert_idx]->type = CONTAINER_ARRAY;
        dv->containers[insert_idx]->capacity = capacity;
        dv->containers[insert_idx]->data = malloc(sizeof(uint16_t) * capacity);
        if (!dv->containers[insert_idx]->data) {
            fprintf(stderr, "[FATAL ERROR] container->data failed to allocate\n");
            exit(EXIT_FAILURE);
        }

        // now container finally exists
        container = dv->containers[insert_idx];
    }

    if (container->type == CONTAINER_ARRAY) {
        if (container->cardinality < 4096) {
            // add the element
            uint16_t *array = (uint16_t *)container->data;
            size_t insert_idx = 0;
            bool found = false;
            for (; insert_idx < container->cardinality; insert_idx++) {
                if (array[insert_idx] == low_key) {
                    // already there, nothing to do
                    found = true;
                    break;
                } else if (array[insert_idx] > low_key) {
                    // shortcircuit
                    break;
                };
            }

            if (!found) {
                // insert
                // do we need to grow?
                if (container->capacity <= container->cardinality) {
                    // double size
                    container->capacity *= 2;
                    void *temp = realloc(array, sizeof(uint16_t) * container->capacity);
                    if (!temp) {
                        fprintf(stderr, "[FATAL ERROR] container->data failed to allocate\n");
                        exit(EXIT_FAILURE);
                    }
                    container->data = temp;
                    // this probably can be cleaner
                    array = temp;
                }

                long num_bytes = (container->cardinality - insert_idx) * sizeof(uint16_t);
                memmove(&array[insert_idx + 1], &array[insert_idx], num_bytes);
                array[insert_idx] = low_key;
                container->cardinality++;
                dv->deleted_count++;
            }

        } else {
            // promote to bitmap
            uint64_t *bitmap = calloc(1024, sizeof(uint64_t));
            uint16_t *array = (uint16_t *)container->data;

            for (size_t i = 0; i < container->cardinality; i++) {
                uint16_t val = array[i];
                bitmap[val / 64] |= (1ULL << (val % 64));
            }
            free(array);
            container->data = bitmap;
            container->type = CONTAINER_BITMAP;
        }
    }

    if (container->type == CONTAINER_BITMAP) {
        uint64_t *bitmap = (uint64_t *)container->data;
        uint64_t mask = (1ULL << (low_key % 64));
        if ((bitmap[low_key / 64] & mask) == 0) {
            bitmap[low_key / 64] |= mask;
            container->cardinality++;
            dv->deleted_count++;
        }
    }

    if (container->type == CONTAINER_RUN) {
        fprintf(stderr, "[FATAL ERROR] container->type=%d not supported\n", container->type);
        exit(EXIT_FAILURE);
    }
}

uint64_t deletion_vector_get_cardinality(const deletion_vector_t *dv) { return dv->deleted_count; }

void deletion_vector_free(deletion_vector_t *dv) {
    for (size_t i = 0; i < dv->num_containers; i++) {
        free(dv->containers[i]->data);
        free(dv->containers[i]);
    }
    free(dv->containers);
    free(dv->keys);
    free(dv);
}

bool deletion_vector_is_deleted(const deletion_vector_t *dv, uint32_t row_id) {
    uint16_t high_key = row_id >> 16;
    uint16_t low_key = row_id & 0xFFFF;

    // find the container
    roaring_container_t *container = NULL;
    size_t insert_idx = 0;
    for (; insert_idx < dv->num_containers; insert_idx++) {
        if (dv->keys[insert_idx] == high_key) {
            container = dv->containers[insert_idx];
            break;
        } else if (dv->keys[insert_idx] > high_key)
            break;
    }

    if (!container)
        return false;

    if (container->type == CONTAINER_ARRAY) {
        uint16_t *array = (uint16_t *)container->data;
        for (size_t i = 0; i < container->cardinality; i++) {
            if (array[i] == low_key) {
                return true;
            } else if (array[i] > low_key) {
                return false;
            }
        }
        return false;
    } else if (container->type == CONTAINER_BITMAP) {
        uint64_t *bitmap = (uint64_t *)container->data;
        uint64_t mask = (1ULL << (low_key % 64));
        return (bitmap[low_key / 64] & mask) != 0;
    }
    return false;
}

uint64_t deletion_vector_get_simd_mask(const deletion_vector_t *dv, uint32_t start_row) {
    uint16_t high_key = start_row >> 16;
    uint16_t low_key = start_row & 0xFFFF;

    // find the container
    roaring_container_t *container = NULL;
    size_t insert_idx = 0;
    for (; insert_idx < dv->num_containers; insert_idx++) {
        if (dv->keys[insert_idx] == high_key) {
            container = dv->containers[insert_idx];
            break;
        } else if (dv->keys[insert_idx] > high_key)
            break;
    }
    if (!container)
        return 0;
    if (container->type == CONTAINER_ARRAY) {
        uint64_t out_mask = 0;
        uint16_t *array = (uint16_t *)container->data;
        for (size_t i = 0; i < container->cardinality; i++) {
            if (array[i] >= low_key && array[i] < low_key + 64) {
                out_mask |= (1ULL << (array[i] - low_key));
            } else if (array[i] >= low_key + 64) {
                break;
            }
        }
        return out_mask;
    } else if (container->type == CONTAINER_BITMAP) {
        uint64_t *bitmap = (uint64_t *)container->data;
        return bitmap[low_key / 64];
    }
    return 0;
}
