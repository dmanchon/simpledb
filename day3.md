# Building a Database Engine in C11 — Day 3: Zero-Copy Memory-Mapped Files (`mmap`) & OS Page Cache

> **Series**: *Building `simpledb` — A High-Performance Columnar Engine in C11*  
> **Reading Time**: 8 minutes

---

Welcome to **Day 3** of building `simpledb` in **Plain C11**!

On **Day 1** and **Day 2**, we built 64-byte aligned memory allocators, dynamic array buffers (`buffer_i32_t`), and zero-copy typed slices (`slice_i32_t`).

Today, on **Day 3**, we solve the ultimate I/O bottleneck in storage engines: **How do we read multi-gigabyte column files from disk into memory without paying the cost of file-read system calls or kernel-to-user buffer copying?**

We construct **Zero-Copy Memory-Mapped Files (`mmap_file_t`)** and use POSIX `madvise` for hardware-level OS page prefetching.

---

## 1. The I/O Problem: Standard File I/O (`fread`/`read`) vs. `mmap`

When an analytical engine queries a 10-Gigabyte column file using standard POSIX file I/O (`read()` / `fread()`):

```
Standard read() I/O (2 Memory Copies + Syscall Overhead):
[ NVMe SSD / Disk ] ──> [ Kernel Page Cache ] ──> (CPU Copying) ──> [ User Space Buffer ]
                                                                        (Pay 10GB RAM Copy Cost!)
```

### 3 Major Downsides of Standard `read()` File I/O:
1. **Double Memory Copying**: The OS kernel reads bytes from disk into the Kernel Page Cache, and then **copies every single byte a second time** into your application's heap buffer.
2. **Context Switching Overhead**: Issuing thousands of `read()` system calls causes frequent CPU context switches between User Mode and Kernel Mode.
3. **RAM Memory Doubling**: Storing data in both the Kernel Page Cache and your application's heap doubles your system RAM footprint.

---

## 2. The Solution: Zero-Copy Memory-Mapping (`mmap`)

POSIX **`mmap()`** instructs the Operating System's Virtual Memory Manager (VMM) to map a file's disk sectors directly into your application's 64-bit virtual address space:

```
Zero-Copy mmap() (0 Memory Copies + 0 Syscall Overhead):
[ NVMe SSD / Disk ] ─────────────── Virtual Memory Mapping ───────────────► [ const int32_t* data ]
                                                                              (Zero Bytes Copied!)
```

### Why `mmap()` Wins for Columnar Databases:
* **0 Bytes Copied**: The pointer (`const int32_t* data`) points directly to the mapped page cache. The CPU reads disk bytes straight from memory with zero copying.
* **Demand Paging**: Pages are loaded from disk into RAM asynchronously only when accessed by CPU instructions.
* **Kernel Page Cache Sharing**: Multiple query worker threads (or processes) read from the exact same physical memory pages without memory duplication.

---

## 3. POSIX `mmap()` & OS Prefetching (`madvise`) Mechanics

To achieve peak memory bandwidth, we use `mmap()` combined with **`madvise()`**:

```c
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
```

### POSIX `mmap` Call:
```c
void* addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
```
* `PROT_READ`: Maps memory as read-only (prevents accidental disk file corruption).
* `MAP_SHARED`: Shares physical page cache with OS.

### OS Prefetching with `madvise()`:
```c
madvise(addr, file_size, MADV_WILLNEED | MADV_SEQUENTIAL);
```
* **`MADV_SEQUENTIAL`**: Informs the kernel OS page scheduler that column array scans read data sequentially. The OS aggressively prefetches future disk pages into CPU L3/RAM ahead of execution!
* **`MADV_WILLNEED`**: Prompts the kernel to begin background asynchronous read-ahead immediately.

---

## 4. Designing the C11 Struct: `mmap_file_t`

We encapsulate disk mapping lifecycle in [include/simpledb/mmap_file.h](file:///Users/dmanchon/repos/cdb/include/simpledb/mmap_file.h):

```c
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "simpledb/slice.h"

// Memory-Mapped File Handle Struct
typedef struct {
    const void* data;  // Read-only pointer mapped directly to disk page cache
    size_t      size;  // Total size of mapped file in bytes
    int         fd;    // Underlying POSIX file descriptor
} mmap_file_t;

// Open and memory-map a binary column file
mmap_file_t* mmap_file_open(const char* filepath);

// Unmap memory and close file descriptor
void mmap_file_close(mmap_file_t* file);

// Convert mapped memory file directly into a Zero-Copy Typed Slice!
static inline slice_i32_t mmap_file_as_slice_i32(const mmap_file_t* file) {
    if (!file || !file->data) return (slice_i32_t){ .data = NULL, .len = 0 };
    return (slice_i32_t){
        .data = (const int32_t*)file->data,
        .len  = file->size / sizeof(int32_t)
    };
}
```

---

## 5. Architectural Comparison: Standard `read()` vs. `mmap()`

| Feature | Standard File I/O (`read` / `fread`) | Zero-Copy `mmap()` |
| :--- | :--- | :--- |
| **Data Memory Copies** | **2 Copies** (Disk → Page Cache → User Buffer) | **0 Copies** (Mapped directly to Virtual Memory) |
| **System Call Overhead** | **High** (`read()` calls on every chunk) | **Zero** (1 initial `mmap()` syscall) |
| **RAM Footprint** | **2x Memory Bloat** (Kernel Cache + Application Heap) | **1x Minimal Footprint** (Shared Page Cache) |
| **Slice Interop** | Requires `malloc` + `fread` + `free` | **Instant**: Pointer cast to `slice_i32_t` in 0.00ms |
| **Page Prefetching** | Manual buffer tuning required | **Automated**: OS hardware prefetching via `madvise` |

---

## Key Takeaways from Day 3

* **Zero-Copy Disk I/O**: `mmap()` maps binary column files straight into 64-bit virtual memory address space with zero byte copying.
* **Unified Slice Interface**: A mapped `mmap_file_t` converts directly into a `slice_i32_t` window view in 0 nanoseconds—allowing physical SIMD query operators to run identically over RAM buffers or disk files!
* **OS Mechanical Sympathy**: Using `madvise(MADV_SEQUENTIAL)` leverages OS kernel read-ahead for max NVMe/SSD bandwidth.

*Next up in Day 4: Micro-Benchmarking Memory Bandwidth (GB/s) & Build System Optimizations!*
