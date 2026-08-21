
Disk I/O represents the primary performance bottleneck in columnar storage engines. In `simpledb`, memory-mapped files (`mmap`) eliminate kernel-to-user buffer copies, exposing binary column files directly as typed slices.

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
| Page Prefetching | Requires manual buffer management | Automated kernel read-ahead via `madvise` |

---

> **Source Code**: The full C11 codebase for this project is available on GitHub at [dmanchon/simpledb](https://github.com/dmanchon/simpledb).
