
Welcome to Day 12! Today we are tackling **Free-Text Search (FTS)**. Traditional OLAP databases were notoriously bad at searching unstructured text (like  application logs, JSON payloads, or event messages). You typically had to export data to Elasticsearch. But modern analytical databases now embed native search capabilities.

## 1. What are ClickHouse and DuckDB doing?

### ClickHouse
ClickHouse approaches text search in two primary ways:
1. **Data Skipping Indexes (Bloom Filters)**: The classic ClickHouse approach uses `tokenbf_v1` (Token Bloom Filter) and `ngrambf_v1` (N-Gram Bloom Filter). When it ingests text, it tokenizes the words, hashes them, and populates a Bloom Filter for each Granule. If you query `WHERE message LIKE '%timeout%'`, it checks the Bloom Filter for that term. If the bit is `0`, it skips the Granule entirely. If `1`, it reads the Granule and performs a slow string matching.
2. **Inverted Indexes**: Recently, ClickHouse introduced experimental support for genuine Inverted Indexes (`INDEX fts ... TYPE inverted(2)`). Instead of probabilistic bloom filters, it builds a mapping from terms (words) to the Granules/Row IDs where they appear, dramatically speeding up exact word matches and enabling relevance scoring.

### DuckDB
DuckDB handles text search via its `fts` (Full-Text Search) extension. 
When you create an FTS index, DuckDB creates hidden relational tables under the hood that act as an inverted index. It tokenizes the documents, stems the words (using algorithms like the Porter stemmer), removes stop words, and stores the term frequencies. At query time, DuckDB uses the **BM25 algorithm** (Best Matching 25) to calculate a relevance score for each row based on term frequency (how often the word appears in the row) and inverse document frequency (how rare the word is across the whole table).

## 2. Methodology

### Text Tokenization
Prior to indexing, unstructured text must be tokenized. Tokenization involves splitting the text by whitespace and punctuation. Further processing includes stemming (reducing words to their base or root form, such as using the Porter stemmer) and the removal of stop words (common words that provide little semantic value, like 'the' or 'and').

### BM25 Scoring Formula
The BM25 formula calculates a relevance score for a document $D$ given a query $Q$ containing keywords $q_1, ..., q_n$:

Score(D, Q) = \sum_{i=1}^{n} IDF(q_i) \cdot rac{f(q_i, D) \cdot (k_1 + 1)}{f(q_i, D) + k_1 \cdot (1 - b + b \cdot rac{|D|}{avgDL})}

Where:
*   $f(q_i, D)$ is the term frequency of $q_i$ in document $D$.
*   $|D|$ is the length of the document in words.
*   $avgDL$ is the average document length in the text collection.
*   $k_1$ and $b$ are free parameters, usually chosen as $k_1 \in [1.2, 2.0]$ and $b = 0.75$.
*   $IDF(q_i)$ is the inverse document frequency of the query term.

## 2. What do we need to do exactly?

For `simpledb`, we are going to build a native **Inverted Block Index** combined with **BM25 Scoring**. We want this to be extremely lean and hardware-efficient in C11.

Instead of pointing words to individual Row IDs (which takes  amounts of memory and doesn't fit our columnar vector design), we will point words to **Granules** (Blocks of 8,192 rows),  integrating with the Sparse Index Pruner we built on Day 10.

Here is the exact architecture we need to build:

1. **The Tokenizer**: A function that takes a text column, splits it by whitespace and punctuation, and extracts terms.
2. **The Inverted Index Structure**: A hash map (or sorted array) where the `key` is the term (e.g., "timeout") and the `value` is a `bitmap_t` representing the Granules that contain this term.
3. **BM25 Scorer**: A struct that tracks Document Lengths (DL) and Average Document Length (avgDL) to calculate the BM25 formula when a user searches for a term.

## 3. The Architecture (Headers)

We need to add these definitions. Following our strict documentation rules, here is the header for our FTS module (`include/simpledb/fts.h`):

```c
// include/simpledb/fts.h

#pragma once
#include <stdint.h>
#include <stddef.h>
#include "simpledb/slice.h"
#include "simpledb/bitmap.h"

// Basic Inverted Index mapping a string term to a bitmap of Granules
typedef struct {
    char** terms;          // Array of unique terms
    bitmap_t** postings;   // Array of pointers to bitmaps (1 bitmap per term)
    size_t num_terms;
    size_t capacity;
} inverted_index_t;

// BM25 Scoring Metadata
typedef struct {
    inverted_index_t* index;
    
    // Total number of rows/documents
    size_t total_docs;
    
    // Average document length (number of tokens)
    double avg_dl;
    
    // Array of lengths for each document (row)
    uint32_t* doc_lengths; 
    
    // BM25 Tuning Parameters (typically k1 = 1.2, b = 0.75)
    double k1;
    double b;
} bm25_scorer_t;

// 1. Initialize an inverted index
inverted_index_t* inverted_index_create(void);

// 2. Tokenize a text string and add it to the index for a specific Granule ID
void inverted_index_add_document(inverted_index_t* idx, const char* text, size_t granule_id);

// 3. Search the index for a term, returning a bitmap of Granules that contain it.
// This integrates  with Day 10's 2-Tier Pruner!
bitmap_t* inverted_index_search(inverted_index_t* idx, const char* term);

// 4. Initialize a BM25 scorer wrapper around an index
bm25_scorer_t* bm25_scorer_create(inverted_index_t* idx, double k1, double b);

// 5. Score a specific row for a query term
double bm25_score_row(bm25_scorer_t* scorer, size_t row_id, const char* term, uint32_t term_freq_in_row);

// 6. Cleanup
void inverted_index_free(inverted_index_t* idx);
void bm25_scorer_free(bm25_scorer_t* scorer);
```

## 4. The Unit Test Mock

Here is how we verify our FTS engine works. We write a test that adds documents and queries the index.

```c
// tests/test_fts.c

#include "simpledb/fts.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

void test_inverted_index(void) {
    inverted_index_t* idx = inverted_index_create();
    
    // Simulate Granule 0
    inverted_index_add_document(idx, "database connection timeout", 0);
    inverted_index_add_document(idx, "timeout occurred", 0);
    
    // Simulate Granule 1
    inverted_index_add_document(idx, "query executed successfully", 1);
    
    // Simulate Granule 2
    inverted_index_add_document(idx, "connection reset by peer", 2);
    
    // Search for "timeout"
    bitmap_t* timeout_mask = inverted_index_search(idx, "timeout");
    assert(timeout_mask != NULL);
    assert(bitmap_is_set(timeout_mask, 0) == 1);
    assert(bitmap_is_set(timeout_mask, 1) == 0);
    assert(bitmap_is_set(timeout_mask, 2) == 0);
    
    // Search for "connection" (should be in Granule 0 and 2)
    bitmap_t* conn_mask = inverted_index_search(idx, "connection");
    assert(conn_mask != NULL);
    assert(bitmap_is_set(conn_mask, 0) == 1);
    assert(bitmap_is_set(conn_mask, 1) == 0);
    assert(bitmap_is_set(conn_mask, 2) == 1);
    
    inverted_index_free(idx);
    printf("FTS Inverted Index test passed!\n");
}
```

By keeping the inverted index tied to Granule IDs instead of Row IDs, we save  amounts of RAM, and the resulting bitmap can be directly bitwise-ANDed with the Sparse Primary Index bitmap from Day 10!
