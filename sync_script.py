import os
import re

def update_file(filepath, new_body):
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Extract frontmatter
    match = re.match(r'^(---\n.*?\n---\n)', content, re.DOTALL)
    if match:
        frontmatter = match.group(1)
    else:
        frontmatter = ''
        
    with open(filepath, 'w') as f:
        f.write(frontmatter + '\n' + new_body.strip() + '\n')

def read_local(filepath):
    with open(filepath, 'r') as f:
        return f.read()

def write_local(filepath, body):
    with open(filepath, 'w') as f:
        f.write(body)

# Day 0
day0_body = """# Day 0: Motivations, Technical Inspirations and Architectural Non-Goals

Before writing the initial C code for `simpledb`, this document outlines the core motivations for the project, its technical ancestry, and the explicit constraints of its design.

## 1. Motivations

Most software applications treat database engines as black boxes accessed via high-level query languages. Building a columnar database kernel from scratch in C11 provides a view into lower-level systems engineering concepts.

### Hardware-Aware Memory Alignment
Modern SIMD vector registers (ARM Neon, Intel AVX2, AVX-512) require data to be aligned to hardware cache line boundaries. Aligning allocations to 64-byte boundaries prevents split cache line fetches, enabling memory bandwidth scans of 40 to 80 GB/s.

### Zero-Copy Memory Mapping
Operating system primitives like `mmap` allow binary column files on disk to be mapped directly into user-space virtual memory addresses, bypassing kernel buffer copies and user-space memory allocations.

### Vectorized Execution Primitives
Structuring data manipulation around array language primitives (compress, gather, group) allows row-by-row iteration to be transformed into tight, branchless vector loops.

### C11 Minimalism
Many C++ database implementations rely on template metaprogramming, class hierarchies, and virtual method dispatch. Plain C11 eliminates template bloat, ensures predictable code generation, and gives explicit control over memory layouts and pointer arithmetic.

## 2. Technical Inspirations

The architecture of `simpledb` draws upon design principles established by four analytical systems.

```
                          simpledb Architectural Ancestry
                                         │
      ┌──────────────────┬───────────────┴───────────────┬──────────────────┐
      │                  │                               │                  │
┌───────────┐    ┌───────────────┐               ┌───────────────┐  ┌─────────────┐
│ ClickHouse│    │  APL / K / q  │               │    DuckDB     │  │Apache Arrow │
└─────┬─────┘    └───────┬───────┘               └───────┬───────┘  └──────┬──────┘
      │                  │                               │                 │
Sparse Primary      Vector Array                    Zero-Copy Slices  Zero-Copy
Index & Granules   compress/gather                 Late Materialization Export
```

### ClickHouse
ClickHouse demonstrates the efficiency of sparse primary indexing. By partitioning data into 8,192-row granules accompanied by mark files, the primary index size is reduced by a factor of 8,192, allowing indexing structures for billion-row tables to fit inside L2 and L3 CPU caches.

### APL and K/q
The array programming lineage introduced by Arthur Whitney proves that data processing is fast when formulated as bulk operations over contiguous primitive vectors (`int32_t*`) rather than object-oriented row tuples.

### DuckDB
DuckDB highlights the benefit of zero-copy typed slices and late materialization. Physical query operators act as read-only window views over underlying column buffers without duplicating data during intermediate query steps.

### Apache Arrow
Apache Arrow establishes a standardized, language-agnostic C Data Interface. Adopting compatible columnar memory layouts allows database buffers to be exposed to external runtimes like Python, Polars, and NumPy without serialization overhead.

## 3. Explicit Non-Goals

To maintain a lean implementation targeted under 2,500 lines of C11, `simpledb` intentionally excludes several traditional database subsystems:

- Commercial Production Readiness: The engine is designed as an educational reference implementation for columnar database internals rather than a general-purpose production store.
- Full ANSI SQL Compliance: The project avoids SQL grammar parsers, query planners, and cost-based optimizers in favor of a minimal vector REPL shell.
- Heavy Transactional Overhead: `simpledb` targets analytical workloads (OLAP) and excludes multi-version concurrency control (MVCC) or ACID logging frameworks required by transactional databases.
- External C++ Dependencies: The codebase consists of POSIX C11, compiling without external third-party dependencies.
"""

# Day 1
day1_body = read_local('/Users/dmanchon/repos/cdb/day1.md')

# Day 2
day2_body = read_local('/Users/dmanchon/repos/cdb/day2.md')
# Extra fixes to Day 2 for style
day2_body = day2_body.replace(
    "In an analytical engine scanning 100 million rows, copying column vectors introduces tens of milliseconds of latency and wastes hundreds of megabytes of memory bandwidth.",
    "In an analytical engine scanning 100 million rows, copying column vectors introduces latency and consumes memory bandwidth."
)

# Day 3
day3_body = read_local('/Users/dmanchon/repos/cdb/day3.md')
day3_body = day3_body.replace(
    "Disk I/O represents the primary performance bottleneck in columnar storage engines. In `simpledb`, memory-mapped files (`mmap`) eliminate kernel-to-user buffer copies, exposing binary column files directly as typed slices.",
    "Disk I/O is a significant cost in columnar storage engines. `simpledb` uses memory-mapped files (`mmap`) to bypass kernel-to-user buffer copies, exposing binary column files directly as typed slices."
)
day3_body = day3_body.replace("automated kernel read-ahead", "automated kernel read ahead")


write_local('/Users/dmanchon/repos/cdb/day0.md', day0_body)
write_local('/Users/dmanchon/repos/cdb/day1.md', day1_body)
write_local('/Users/dmanchon/repos/cdb/day2.md', day2_body)
write_local('/Users/dmanchon/repos/cdb/day3.md', day3_body)

# Update Astro files
astro_dir = '/Users/dmanchon/repos/astro-blog/src/content/blog'
update_file(f'{astro_dir}/day0-motivations-and-influences.md', day0_body)
update_file(f'{astro_dir}/day1-64byte-simd-alignment.md', day1_body)
update_file(f'{astro_dir}/day2-zero-copy-typed-slices.md', day2_body)
update_file(f'{astro_dir}/day3-zero-copy-mmap-files.md', day3_body)

print("Done")
