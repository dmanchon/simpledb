# Day 3: Zero-Copy Memory-Mapped Files (`mmap`) & OS Page Cache

> **Day 3 Branch**: View the exact code for this day at [dmanchon/simpledb (`day3`)](https://github.com/dmanchon/simpledb/tree/day3) ([diff from `day2`](https://github.com/dmanchon/simpledb/compare/day2...day3)).

Disk I/O is a significant cost in columnar storage engines. `simpledb` uses memory-mapped files (`mmap`) to bypass kernel-to-user buffer copies, exposing binary column files directly as typed slices.

## 1. System Call Costs: Standard I/O vs. Memory Mapping

When an analytical engine scans multi-gigabyte column files using standard POSIX file calls (`read` or `fread`), data traverses multiple buffering layers:

```
Standard read() I/O (Two Memory Copies):
[ Storage Disk ] ──> [ Kernel Page Cache ] ──> (CPU Copy) ──> [ User Space Buffer ]
```

Standard file I/O introduces three system overheads:
1. Double Memory Buffering: Data is read from disk into the kernel page cache and subsequently copied into the application's heap allocation.
2. Context Switching: Repeated `read` system calls trigger user-to-kernel context switches.
3. Memory Duplication: Maintaining duplicate data copies in both the kernel page cache and user heap doubles system memory consumption.

## 2. Memory-Mapped File Architecture

POSIX `mmap` instructs the virtual memory manager to map file descriptors directly into the process address space.

```
Zero-Copy mmap() (Zero Copies):
[ Storage Disk ] ─────────────── Virtual Memory Mapping ───────────────► [ const int32_t* data ]
```

Mapping files directly into virtual memory provides three operational advantages:
- Zero-Copy Reads: Pointers reference the kernel page cache directly, allowing CPU SIMD instructions to read column data without user-space buffer allocations.
- Asynchronous Demand Paging: Page tables load data blocks from disk asynchronously when accessed by execution threads.
- Shared Page Caches: Concurrent execution workers inspect identical physical memory pages without memory duplication.

## 3. POSIX `mmap` and `madvise` Execution

Memory mapping is configured using `mmap` and optimized using `madvise` prefetching hints.

```c
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

void* addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
```

- `PROT_READ`: Specifies read-only page protection to prevent accidental file modification.
- `MAP_SHARED`: Shares physical pages with the OS page cache.

```c
madvise(addr, file_size, MADV_WILLNEED | MADV_SEQUENTIAL);
```

- `MADV_SEQUENTIAL`: Signals to the OS page scheduler that data accesses will occur sequentially, triggering aggressive kernel read-ahead.
- `MADV_WILLNEED`: Advises the kernel to begin asynchronous background page reads immediately.

## 4. Interface Definition: `include/simpledb/mmap_file.h`

The `mmap_file_t` structure encapsulates file descriptors and virtual address mappings.

```c
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "simpledb/slice.h"

typedef struct {
    const void* data;
    size_t      size;
    int         fd;
} mmap_file_t;

mmap_file_t* mmap_file_open(const char* filepath);
void mmap_file_close(mmap_file_t* file);

static inline slice_i32_t mmap_file_as_slice_i32(const mmap_file_t* file) {
    if (!file || !file->data) return (slice_i32_t){ .data = NULL, .len = 0 };
    return (slice_i32_t){
        .data = (const int32_t*)file->data,
        .len  = file->size / sizeof(int32_t)
    };
}
```

## 5. Architectural Comparison: Standard File I/O vs. `mmap`

| Feature | Standard File I/O (`read` / `fread`) | Zero-Copy `mmap()` |
| :- | :- | :- |
| Data Memory Copies | Two copies (Disk to Page Cache to Heap) | Zero copies (Mapped directly to Virtual Memory) |
| System Call Overhead | High (repeated syscalls per chunk) | Zero (single initial `mmap` call) |
| Memory Footprint | High (Kernel Cache plus User Heap) | Low (Shared kernel page cache) |
| Slice Conversion | Requires heap allocation and copy | Instant (Pointer cast to `slice_i32_t`) |
| Page Prefetching | Requires manual buffer management | Automated kernel read ahead via `madvise` |

## 6. Unit Test Verification

```c
// [tests/test_db.c]
static void test_mmap_file(void) {
    const char *tmp_filename = "test_column.bin";

    // Write 100 integers to temporary column file
    FILE *f = fopen(tmp_filename, "wb");
    assert(f != NULL);
    for (int32_t i = 0; i < 100; i++) {
        int32_t val = (i + 1) * 7;
        fwrite(&val, sizeof(int32_t), 1, f);
    }
    fclose(f);

    // Open via zero-copy mmap
    mmap_file_t *mfile = mmap_file_open(tmp_filename);
    assert(mfile != NULL);
    assert(mfile->size == 100 * sizeof(int32_t));
    assert(mfile->data != NULL);

    // Convert to typed slice
    slice_i32_t slice = mmap_file_as_slice_i32(mfile);
    assert(slice.len == 100);
    assert(slice_i32_at(slice, 0) == 7);
    assert(slice_i32_at(slice, 49) == 350);
    assert(slice_i32_at(slice, 99) == 700);
    (void)slice;

    // Close and remove file
    mmap_file_close(mfile);
    remove(tmp_filename);
}
```

## 7. Final Implementation (`src/mmap_file.c`)

```c
#include "simpledb/mmap_file.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

mmap_file_t *mmap_file_open(const char *filepath) {
    if (!filepath)
        return NULL;

    // open the file
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "[simpledb error] Failed to open column file '%s': %s (errno=%d)\n",
                filepath, strerror(errno), errno);
        return NULL;
    }

    // get the file size
    struct stat st;
    if (fstat(fd, &st) == -1) {
        fprintf(stderr, "[simpledb error] Failed to fstat column file '%s': %s (errno=%d)\n",
                filepath, strerror(errno), errno);
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;

    // create the mmap
    void *addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        fprintf(stderr,
                "[simpledb error] Failed to mmap column file '%s' (%zu bytes): %s (errno=%d)\n",
                filepath, file_size, strerror(errno), errno);
        close(fd);
        return NULL;
    }

    mmap_file_t *file = (mmap_file_t *)malloc(sizeof(mmap_file_t));
    file->data = addr;
    file->size = file_size;
    file->fd = fd;

    return file;
}

void mmap_file_close(mmap_file_t *file) {
    if (!file)
        return;

    if (file->data && file->size > 0) {
        munmap((void *)file->data, file->size);
    }

    if (file->fd != -1) {
        close(file->fd);
    }

    free(file);
}
```

---

> **Day 3 Branch**: View the exact code for this day at [dmanchon/simpledb (`day3`)](https://github.com/dmanchon/simpledb/tree/day3) ([diff from `day2`](https://github.com/dmanchon/simpledb/compare/day2...day3)).
