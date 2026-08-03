#!/usr/bin/env python3
import os
import re

CDB_DIR = "/Users/dmanchon/repos/cdb"
BLOG_CONTENT_DIR = "/Users/dmanchon/repos/astro-blog/src/content/blog"

POST_METADATA = {
    "day0.md": {
        "slug": "day0-motivations-and-influences.md",
        "title": "Day 0: Motivations, Technical Inspirations & Non-Goals",
        "description": "Why build a columnar analytical database engine in C11 from scratch? Architectural influences from ClickHouse, APL/K, DuckDB, and Apache Arrow.",
        "pubDate": "2026-08-01",
        "tags": ["c11", "databases", "architecture", "clickhouse"]
    },
    "day1.md": {
        "slug": "day1-64byte-simd-alignment.md",
        "title": "Day 1: 64-Byte SIMD Alignment & Dynamic Column Buffers",
        "description": "How modern analytical engines achieve 40-80 GB/s scans. Why 64-byte alignment prevents cache-line splits and how to construct zero-overhead C11 column buffers.",
        "pubDate": "2026-08-01",
        "tags": ["c11", "simd", "performance", "memory"]
    },
    "day2.md": {
        "slug": "day2-zero-copy-typed-slices.md",
        "title": "Day 2: Zero-Copy Typed Slices & C11 _Generic Polymorphism",
        "description": "How physical query operators read column data with 0 bytes copied. Implementing typed slices and C11 compile-time generic dispatch.",
        "pubDate": "2026-08-01",
        "tags": ["c11", "slices", "zero-copy", "polymorphism"]
    },
    "day3.md": {
        "slug": "day3-zero-copy-mmap-files.md",
        "title": "Day 3: Zero-Copy Memory-Mapped Files (mmap) & OS Page Cache",
        "description": "Reading multi-gigabyte column files with zero byte copying. POSIX mmap, madvise prefetching, and mmap_file_t architecture.",
        "pubDate": "2026-08-02",
        "tags": ["c11", "mmap", "posix", "io"]
    },
    "day4.md": {
        "slug": "day4-build-system-and-benchmarking.md",
        "title": "Day 4: Build System, Compiler Optimizations & POSIX Micro-Benchmarking",
        "description": "How optimization flags (-O3 -march=native) and high-resolution POSIX timing (clock_gettime) impact hardware memory bandwidth scanning throughput.",
        "pubDate": "2026-08-02",
        "tags": ["c11", "benchmarking", "optimizations", "posix", "performance"]
    }
}

URL_MAP = {
    "day0.md": "/blog/day0-motivations-and-influences/",
    "day1.md": "/blog/day1-64byte-simd-alignment/",
    "day2.md": "/blog/day2-zero-copy-typed-slices/",
    "day3.md": "/blog/day3-zero-copy-mmap-files/",
    "day4.md": "/blog/day4-build-system-and-benchmarking/",
}

def clean_latex_math(text):
    # 1. Clean fractions: \frac{A}{B} -> (A) / (B)
    def clean_frac(match):
        num = match.group(1).strip()
        den = match.group(2).strip()
        num = re.sub(r"\\(text|mandatory|mathbf|mathit|mathrm)\{([^}]+)\}", r"\2", num)
        den = re.sub(r"\\(text|mandatory|mathbf|mathit|mathrm)\{([^}]+)\}", r"\2", den)
        return f"({num}) / ({den})"

    text = re.sub(r"\\frac\{([^}]+)\}\{([^}]+)\}", clean_frac, text)

    # 2. Clean escaped underscores, percents, LaTeX en-dashes
    text = text.replace(r"\_", "_")
    text = text.replace(r"\%", "%")
    text = text.replace(r"\text{--}", "-")

    # 3. Clean standalone hyphens/dashes on blank lines so they don't trigger Setext H2 headings
    text = re.sub(r"^\s*-{1,2}\s*$", "", text, flags=re.MULTILINE)

    # 4. Clean LaTeX symbols & arrows
    text = text.replace(r"\rightarrow", "→")
    text = text.replace(r"\leftarrow", "←")
    text = text.replace(r"\sim", "~")
    text = text.replace(r"\times", "x")

    # 5. Clean superscript powers like 10^9 -> 10⁹
    text = text.replace("10^9", "10⁹")
    text = text.replace("10^6", "10⁶")

    # 6. Clean \text{...}, \textbf{...}, \mandatory{...}, \mathbf{...}, \mathit{...}
    text = re.sub(r"\\(text|textbf|mandatory|mathbf|mathit|mathrm)\{([^}]+)\}", r"\2", text)

    # 7. Strip enclosing multi-line $$ ... $$ and inline $ ... $
    text = re.sub(r"\$\$\s*([\s\S]*?)\s*\$\$", r"\1", text)
    text = re.sub(r"\$([^$]+)\$", r"\1", text)

    return text

def rewrite_internal_links(text):
    for filename, url in URL_MAP.items():
        text = text.replace(f"({filename})", f"({url})")
        text = text.replace(f"(./{filename})", f"({url})")
    return text

def sync():
    os.makedirs(BLOG_CONTENT_DIR, exist_ok=True)

    # Remove any orphaned blog files in astro-blog not present in POST_METADATA
    valid_slugs = {meta['slug'] for meta in POST_METADATA.values()}
    for filename in os.listdir(BLOG_CONTENT_DIR):
        if filename.endswith(".md") and filename not in valid_slugs:
            orphaned_path = os.path.join(BLOG_CONTENT_DIR, filename)
            os.remove(orphaned_path)
            print(f"Removed orphaned blog post: {orphaned_path}")

    for day_file, meta in POST_METADATA.items():
        src_path = os.path.join(CDB_DIR, day_file)
        if not os.path.exists(src_path):
            continue
        
        with open(src_path, "r", encoding="utf-8") as f:
            content = f.read()

        # Clean LaTeX math symbols and standalone single hyphens
        content = clean_latex_math(content)

        # Write clean markdown back to cdb source file
        with open(src_path, "w", encoding="utf-8") as f:
            f.write(content)

        # Rewrite relative dayX.md links to Astro blog URL slugs
        blog_content = rewrite_internal_links(content)

        # Remove top level # H1 if present
        content_lines = blog_content.splitlines()
        if content_lines and content_lines[0].startswith("# "):
            blog_content = "\n".join(content_lines[1:]).lstrip()

        tags_str = ", ".join([f"'{t}'" for t in meta['tags']])
        frontmatter = f"""---
title: '{meta['title']}'
description: '{meta['description']}'
pubDate: '{meta['pubDate']}'
tags: [{tags_str}]
---

"""
        target_path = os.path.join(BLOG_CONTENT_DIR, meta['slug'])
        with open(target_path, "w", encoding="utf-8") as f:
            f.write(frontmatter + blog_content)
        
        print(f"Cleaned & Synced {day_file} -> {target_path}")

if __name__ == "__main__":
    sync()
