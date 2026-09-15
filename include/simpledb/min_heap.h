#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// A heap node pairing a 64-bit scalar sort key with an opaque payload pointer
typedef struct {
    int64_t key;      // Primitive sort key (compared directly for maximum performance)
    void *payload;    // Opaque user context (e.g. stream pointer, run descriptor, row pointer)
} heap_node_t;

// Contiguous flat-array Min-Heap Priority Queue
typedef struct {
    heap_node_t *data; // Contiguous array storing the complete binary tree
    size_t size;       // Number of active elements in the heap
    size_t capacity;   // Maximum allocated capacity
} min_heap_t;

// Lifecycle management
min_heap_t *min_heap_create(size_t capacity);
void min_heap_destroy(min_heap_t *heap);

// Core array navigation and sift kernels
void min_heap_sift_up(min_heap_t *heap, size_t idx);
void min_heap_sift_down(min_heap_t *heap, size_t idx);

// Ergonomic Priority Queue API
int min_heap_push(min_heap_t *heap, int64_t key, void *payload);
bool min_heap_peek(const min_heap_t *heap, heap_node_t *out_node);
bool min_heap_pop(min_heap_t *heap, heap_node_t *out_node);
bool min_heap_replace_top(min_heap_t *heap, int64_t key, void *payload);
