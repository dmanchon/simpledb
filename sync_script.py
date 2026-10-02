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
        
    # Strip out the top-level H1 header (e.g., # Day 14: ...) to avoid double titles
    new_body_stripped = re.sub(r'^#\s+[^\n]*\n+', '', new_body.lstrip(), count=1)
        
    with open(filepath, 'w') as f:
        f.write(frontmatter + '\n' + new_body_stripped.strip() + '\n')

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
day1_body = read_local('/Users/dmanchon/repos/simpledb/day1.md')

# Day 2
day2_body = read_local('/Users/dmanchon/repos/simpledb/day2.md')
# Extra fixes to Day 2 for style
day2_body = day2_body.replace(
    "In an analytical engine scanning 100 million rows, copying column vectors introduces tens of milliseconds of latency and wastes hundreds of megabytes of memory bandwidth.",
    "In an analytical engine scanning 100 million rows, copying column vectors introduces latency and consumes memory bandwidth."
)

# Day 3
day3_body = read_local('/Users/dmanchon/repos/simpledb/day3.md')
day3_body = day3_body.replace(
    "Disk I/O represents the primary performance bottleneck in columnar storage engines. In `simpledb`, memory-mapped files (`mmap`) eliminate kernel-to-user buffer copies, exposing binary column files directly as typed slices.",
    "Disk I/O is a significant cost in columnar storage engines. `simpledb` uses memory-mapped files (`mmap`) to bypass kernel-to-user buffer copies, exposing binary column files directly as typed slices."
)
day3_body = day3_body.replace("automated kernel read-ahead", "automated kernel read ahead")


write_local('/Users/dmanchon/repos/simpledb/day0.md', day0_body)
write_local('/Users/dmanchon/repos/simpledb/day1.md', day1_body)
write_local('/Users/dmanchon/repos/simpledb/day2.md', day2_body)
write_local('/Users/dmanchon/repos/simpledb/day3.md', day3_body)

# Day 13
day13_body = read_local('/Users/dmanchon/repos/simpledb/day13.md')
write_local('/Users/dmanchon/repos/simpledb/day13.md', day13_body)

# Day 14
day14_body = read_local('/Users/dmanchon/repos/simpledb/day14.md')
write_local('/Users/dmanchon/repos/simpledb/day14.md', day14_body)

# Day 14 Bis
day14bis_body = read_local('/Users/dmanchon/repos/simpledb/day14bis.md') if os.path.exists('/Users/dmanchon/repos/simpledb/day14bis.md') else ""
if day14bis_body:
    write_local('/Users/dmanchon/repos/simpledb/day14bis.md', day14bis_body)

# Day 15
day15_body = read_local('/Users/dmanchon/repos/simpledb/day15.md') if os.path.exists('/Users/dmanchon/repos/simpledb/day15.md') else ""
if day15_body:
    write_local('/Users/dmanchon/repos/simpledb/day15.md', day15_body)

# Day 16
day16_body = read_local('/Users/dmanchon/repos/simpledb/day16.md') if os.path.exists('/Users/dmanchon/repos/simpledb/day16.md') else ""
if day16_body:
    write_local('/Users/dmanchon/repos/simpledb/day16.md', day16_body)

# Update Astro files
astro_dir = '/Users/dmanchon/repos/astro-blog/src/content/blog'
update_file(f'{astro_dir}/day0-motivations-and-influences.md', day0_body)
update_file(f'{astro_dir}/day1-64byte-simd-alignment.md', day1_body)
update_file(f'{astro_dir}/day2-zero-copy-typed-slices.md', day2_body)
update_file(f'{astro_dir}/day3-zero-copy-mmap-files.md', day3_body)
update_file(f'{astro_dir}/day13-deletion-vectors.md', day13_body)
update_file(f'{astro_dir}/day14.md', day14_body)
if day14bis_body:
    day14bis_path = f'{astro_dir}/day14bis.md'
    if not os.path.exists(day14bis_path):
        with open(day14bis_path, 'w') as f:
            f.write('---\ntitle: "Day 14 Bis: Table Flush & Immutable Part Serialization"\npubDate: 2026-08-28\ndescription: "Implementing streaming disk flush from RAM columnar buffers to immutable .bin files and managing part metadata."\n---\n\n')
    update_file(day14bis_path, day14bis_body)
if day15_body:
    day15_path = f'{astro_dir}/day15.md'
    if not os.path.exists(day15_path):
        with open(day15_path, 'w') as f:
            f.write('---\ntitle: "Day 15: External Merge Sort (Disk-Spilling ORDER BY)"\npubDate: 2026-08-29\ndescription: "Implementing out-of-core external merge sort with fixed memory budgets and K-way streaming min-heap merge."\n---\n\n')
    update_file(day15_path, day15_body)
if day16_body:
    day16_path = f'{astro_dir}/day16.md'
    if not os.path.exists(day16_path):
        with open(day16_path, 'w') as f:
            f.write('---\ntitle: "Day 16: Partitioned Grace Hash Aggregation (Disk Spilling GROUP BY)"\npubDate: 2026-08-30\ndescription: "Implementing partitioned grace hash aggregation for high-cardinality GROUP BY with bounded memory."\n---\n\n')
    update_file(day16_path, day16_body)

day17_body = read_local('/Users/dmanchon/repos/simpledb/day17.md') if os.path.exists('/Users/dmanchon/repos/simpledb/day17.md') else ""
if day17_body:
    day17_path = f'{astro_dir}/day17.md'
    if not os.path.exists(day17_path):
        with open(day17_path, 'w') as f:
            f.write('---\ntitle: "Day 17: The Multi-Part Table Scanner (Lazy Data Loading & Granule Pruning)"\npubDate: 2026-08-31\ndescription: "Building a Volcano-model Table Scanner to stream data from disk parts and memory buffers."\n---\n\n')
    update_file(day17_path, day17_body)

day18_body = read_local('/Users/dmanchon/repos/simpledb/day18.md') if os.path.exists('/Users/dmanchon/repos/simpledb/day18.md') else ""
if day18_body:
    day18_path = f'{astro_dir}/day18.md'
    if not os.path.exists(day18_path):
        with open(day18_path, 'w') as f:
            f.write('---\ntitle: "Day 18: APL / K Vectorized Physical Execution Pipeline"\npubDate: 2026-09-01\ndescription: "Assembling execution pipeline executing scan, filter, gather, aggregate."\n---\n\n')
    update_file(day18_path, day18_body)

day19_body = read_local('/Users/dmanchon/repos/simpledb/day19.md') if os.path.exists('/Users/dmanchon/repos/simpledb/day19.md') else ""
if day19_body:
    day19_path = f'{astro_dir}/day19.md'
    if not os.path.exists(day19_path):
        with open(day19_path, 'w') as f:
            f.write('---\ntitle: "Day 19: The MergeTree Part Compactor"\npubDate: 2026-09-02\ndescription: "Merging multiple immutable on-disk parts into a single consolidated, deduplicated part."\n---\n\n')
    update_file(day19_path, day19_body)

print("Done")


