#include "simpledb/allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *aligned_malloc(size_t bytes) {
  if (bytes == 0) {
    return NULL;
  }
  void *buffer = NULL;

  int status = posix_memalign(&buffer, SIMD_ALIGNMENT, bytes);

  if (status != 0) {
    fprintf(stderr,
            "[FATAL ERROR] aligned_malloc failed to allocate %zu bytes (code: "
            "%d)\n",
            bytes, status);
    exit(EXIT_FAILURE);
  }
  return buffer;
}

void aligned_free(void *ptr) {
  if (ptr) {
    free(ptr);
  }
}

buffer_i32_t *buffer_i32_create(size_t initial_capacity) {
  if (initial_capacity == 0) {
    initial_capacity = 16;
  }

  buffer_i32_t *buf = (buffer_i32_t *)malloc(sizeof(buffer_i32_t));
  if (!buf) {
    return NULL;
  }

  buf->data = (int32_t *)aligned_malloc(initial_capacity * sizeof(int32_t));
  buf->capacity = initial_capacity;
  buf->size = 0;
  return buf;
}

void buffer_i32_free(buffer_i32_t *buf) {
  if (buf) {
    if (buf->data) {
      aligned_free(buf->data);
      buf->data = NULL;
    }
    free(buf);
  }
}

void buffer_i32_grow(buffer_i32_t *buf) {
  size_t new_capacity = (buf->capacity == 0) ? 16 : buf->capacity * 2;
  int32_t *new_data = (int32_t *)aligned_malloc(new_capacity * sizeof(int32_t));
  if (!new_data) {
    fprintf(stderr, "[FATAL ERROR] cannot grow buffer from %zu -> %zu\n",
            buf->size, new_capacity);
    exit(EXIT_FAILURE);
  }
  if (buf->data && buf->size > 0) {
    memcpy(new_data, buf->data, buf->size * sizeof(int32_t));
    aligned_free(buf->data);
  }
  buf->data = new_data;
  buf->capacity = new_capacity;
}
