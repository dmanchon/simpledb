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
day0_body = read_local('/Users/dmanchon/repos/cdb/day0.md')

# Day 1
day1_body = read_local('/Users/dmanchon/repos/cdb/day1.md')

# Day 2
day2_body = read_local('/Users/dmanchon/repos/cdb/day2.md')

# Day 3
day3_body = read_local('/Users/dmanchon/repos/cdb/day3.md')

# Days 4 - 9
day4_body = read_local('/Users/dmanchon/repos/cdb/day4.md')
day5_body = read_local('/Users/dmanchon/repos/cdb/day5.md')
day6_body = read_local('/Users/dmanchon/repos/cdb/day6.md')
day7_body = read_local('/Users/dmanchon/repos/cdb/day7.md')
day8_body = read_local('/Users/dmanchon/repos/cdb/day8.md')
day9_body = read_local('/Users/dmanchon/repos/cdb/day9.md')

# Days 10 - 12
day10_body = read_local('/Users/dmanchon/repos/cdb/day10.md')
day11_body = read_local('/Users/dmanchon/repos/cdb/day11.md')
day12_body = read_local('/Users/dmanchon/repos/cdb/day12.md')

# Day 13
day13_body = read_local('/Users/dmanchon/repos/cdb/day13.md')

# Day 14
day14_body = read_local('/Users/dmanchon/repos/cdb/day14.md')

# Day 14 Bis
day14bis_body = read_local('/Users/dmanchon/repos/cdb/day14bis.md') if os.path.exists('/Users/dmanchon/repos/cdb/day14bis.md') else ""

# Day 15
day15_body = read_local('/Users/dmanchon/repos/cdb/day15.md') if os.path.exists('/Users/dmanchon/repos/cdb/day15.md') else ""

# Day 16
day16_body = read_local('/Users/dmanchon/repos/cdb/day16.md') if os.path.exists('/Users/dmanchon/repos/cdb/day16.md') else ""

# Update Astro files
astro_dir = '/Users/dmanchon/repos/astro-blog/src/content/blog'
update_file(f'{astro_dir}/day0-motivations-and-influences.md', day0_body)
update_file(f'{astro_dir}/day1-64byte-simd-alignment.md', day1_body)
update_file(f'{astro_dir}/day2-zero-copy-typed-slices.md', day2_body)
update_file(f'{astro_dir}/day3-zero-copy-mmap-files.md', day3_body)
update_file(f'{astro_dir}/day4-build-system-and-benchmarking.md', day4_body)
update_file(f'{astro_dir}/day5-column-vectors-and-selection-vectors.md', day5_body)
update_file(f'{astro_dir}/day6-high-cardinality-string-dictionaries.md', day6_body)
update_file(f'{astro_dir}/day7-null-bitmaps.md', day7_body)
update_file(f'{astro_dir}/day8-simd-apl-vector-primitives.md', day8_body)
update_file(f'{astro_dir}/day9-clickhouse-style-storage-and-marks.md', day9_body)
update_file(f'{astro_dir}/day10-sparse-primary-index.md', day10_body)
update_file(f'{astro_dir}/day11-column-reader.md', day11_body)
update_file(f'{astro_dir}/day12-free-text-search.md', day12_body)
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

day17_body = read_local('/Users/dmanchon/repos/cdb/day17.md') if os.path.exists('/Users/dmanchon/repos/cdb/day17.md') else ""
if day17_body:
    day17_path = f'{astro_dir}/day17.md'
    if not os.path.exists(day17_path):
        with open(day17_path, 'w') as f:
            f.write('---\ntitle: "Day 17: The Multi-Part Table Scanner (Lazy Data Loading & Granule Pruning)"\npubDate: 2026-08-31\ndescription: "Building a Volcano-model Table Scanner to stream data from disk parts and memory buffers."\n---\n\n')
    update_file(day17_path, day17_body)

day18_body = read_local('/Users/dmanchon/repos/cdb/day18.md') if os.path.exists('/Users/dmanchon/repos/cdb/day18.md') else ""
if day18_body:
    day18_path = f'{astro_dir}/day18.md'
    if not os.path.exists(day18_path):
        with open(day18_path, 'w') as f:
            f.write('---\ntitle: "Day 18: APL / K Vectorized Physical Execution Pipeline"\npubDate: 2026-09-01\ndescription: "Assembling execution pipeline executing scan, filter, gather, aggregate."\n---\n\n')
    update_file(day18_path, day18_body)

day19_body = read_local('/Users/dmanchon/repos/cdb/day19.md') if os.path.exists('/Users/dmanchon/repos/cdb/day19.md') else ""
if day19_body:
    day19_path = f'{astro_dir}/day19.md'
    if not os.path.exists(day19_path):
        with open(day19_path, 'w') as f:
            f.write('---\ntitle: "Day 19: The MergeTree Part Compactor"\npubDate: 2026-09-02\ndescription: "Merging multiple immutable on-disk parts into a single consolidated, deduplicated part."\n---\n\n')
    update_file(day19_path, day19_body)

day20_body = read_local('/Users/dmanchon/repos/cdb/day20.md') if os.path.exists('/Users/dmanchon/repos/cdb/day20.md') else ""
if day20_body:
    day20_path = f'{astro_dir}/day20.md'
    if not os.path.exists(day20_path):
        with open(day20_path, 'w') as f:
            f.write('---\ntitle: "Day 20: Minimal SQL & Query Parser (Lexer, Recursive Descent & AST)"\npubDate: 2026-09-03\ndescription: "Building an analytical SQL Lexer, recursive descent parser, and Abstract Syntax Tree (AST) in C11."\n---\n\n')
    update_file(day20_path, day20_body)

day21_body = read_local('/Users/dmanchon/repos/cdb/day21.md') if os.path.exists('/Users/dmanchon/repos/cdb/day21.md') else ""
if day21_body:
    day21_path = f'{astro_dir}/day21.md'
    if not os.path.exists(day21_path):
        with open(day21_path, 'w') as f:
            f.write('---\ntitle: "Day 21: The Interactive REPL & Write Path (Dynamic Schema Evolution & Streaming NDJSON Ingestion)"\npubDate: 2026-09-04\ndescription: "Building an interactive analytical database REPL, dynamic schema evolution with backfilling, and streaming NDJSON ingestion with yyjson."\n---\n\n')
    update_file(day21_path, day21_body)

day22_body = read_local('/Users/dmanchon/repos/cdb/day22.md') if os.path.exists('/Users/dmanchon/repos/cdb/day22.md') else ""
if day22_body:
    day22_path = f'{astro_dir}/day22.md'
    if not os.path.exists(day22_path):
        with open(day22_path, 'w') as f:
            f.write('---\ntitle: "Day 22: The REPL Read Path & Query Planner (Binding SQL SELECT to the Vectorized Pipeline)"\npubDate: 2026-09-05\ndescription: "Building a Query Planner and Binder that translates SQL SELECT ASTs into physical operator trees for vectorized execution."\n---\n\n')
    update_file(day22_path, day22_body)

print("Done")


