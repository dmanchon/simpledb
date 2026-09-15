#include "simpledb/min_heap.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

min_heap_t *min_heap_create(size_t capacity) {
    min_heap_t *heap = malloc(sizeof(*heap));
    if (!heap) {
        return NULL;
    }
    heap->data = malloc(sizeof(heap_node_t) * capacity);
    if (!heap->data && capacity > 0) {
        free(heap);
        return NULL;
    }
    heap->capacity = capacity;
    heap->size = 0;
    return heap;
}

static inline size_t heap_parent(size_t idx) { return (idx - 1) / 2; }

static inline size_t heap_left(size_t idx) { return 2 * idx + 1; }

static inline size_t heap_right(size_t idx) { return 2 * idx + 2; }

void min_heap_destroy(min_heap_t *heap) {
    free(heap->data);
    free(heap);
}

void min_heap_sift_up(min_heap_t *heap, size_t inserted_node_idx) {
    // keep the min-heap invariant: Parent(Key) <= Children(Key)
    while (inserted_node_idx > 0) {
        size_t parent = heap_parent(inserted_node_idx);
        if (heap->data[inserted_node_idx].key >= heap->data[parent].key)
            break;
        // swap the node
        heap_node_t tmp = heap->data[inserted_node_idx];
        heap->data[inserted_node_idx] = heap->data[parent];
        heap->data[parent] = tmp;

        // continue
        inserted_node_idx = parent;
    }
}

void min_heap_sift_down(min_heap_t *heap, size_t removed_node_idx) {
    // keep the min-heap invariant: Parent(Key) <= Children(Key)
    while (heap_left(removed_node_idx) < heap->size) {
        size_t left = heap_left(removed_node_idx);
        size_t right = heap_right(removed_node_idx);
        size_t smallest = removed_node_idx;

        if (heap->data[left].key < heap->data[smallest].key) {
            smallest = left;
        }

        if (right < heap->size && heap->data[right].key < heap->data[smallest].key) {
            smallest = right;
        }

        if (smallest == removed_node_idx) {
            break;
        }

        heap_node_t tmp = heap->data[removed_node_idx];
        heap->data[removed_node_idx] = heap->data[smallest];
        heap->data[smallest] = tmp;

        // continue
        removed_node_idx = smallest;
    }
}

int min_heap_push(min_heap_t *heap, int64_t key, void *payload) {
    if (!heap || heap->size >= heap->capacity) {
        return -1;
    }

    heap_node_t node = {.payload = payload, .key = key};

    heap->data[heap->size] = node;
    min_heap_sift_up(heap, heap->size);

    heap->size++;

    return 0;
}

bool min_heap_peek(const min_heap_t *heap, heap_node_t *out_node) {
    if (!heap || heap->size == 0 || !out_node) {
        return false;
    }

    *out_node = heap->data[0];
    return true;
}

bool min_heap_pop(min_heap_t *heap, heap_node_t *out_node) {
    if (!heap || heap->size == 0 || !out_node) {
        return false;
    }
    *out_node = heap->data[0];

    heap->size--;
    if (heap->size > 0) {
        heap->data[0] = heap->data[heap->size];
        min_heap_sift_down(heap, 0);
    }

    return true;
}

bool min_heap_replace_top(min_heap_t *heap, int64_t key, void *payload) {
    if (!heap || heap->size == 0) {
        return false;
    }
    heap_node_t node = {.payload = payload, .key = key};
    heap->data[0] = node;
    min_heap_sift_down(heap, 0);
    return true;
}
