# Day 20: Minimal SQL & Query Parser (Lexer, Recursive Descent & AST)

> **Day 20 Branch**: View the exact code for this day at [dmanchon/simpledb (`day20`)](https://github.com/dmanchon/simpledb/tree/day20) ([diff from `day19`](https://github.com/dmanchon/simpledb/compare/day19...day20)).

> [!TIP]
> **Long-Read Engineering Log (~1,280 lines) — TL;DR & Fast Navigation**
> This post is an unabridged Socratic pair-programming log covering our zero-dependency SQL Lexer, Recursive Descent Parser, and AST in C11.
> - **Zero-Allocation Tokenizer (`sql_lexer_next_token`):** Scans raw SQL strings into typed `sql_token_t` tokens without Flex/Bison code bloat or heap allocations.
> - **Recursive Descent AST (`sql_parser_parse`):** Parses analytical `SELECT ... WHERE ... GROUP BY ... ORDER BY`, single-row `INSERT INTO ... VALUES (...)`, and bulk `INSERT INTO ... FROM 'file.json'` into a flat, arena-friendly `ast_query_t` struct.
> - **Want to go straight to the code?** [Jump to the C Header (`include/simpledb/sql_parser.h`)](#1-the-headers), [Jump to the Unit Tests](#2-mocking-and-testing), or [Jump to the Final C11 Implementation (`src/sql_parser.c`)](#3-the-final-listing-srcsql_parserc).

Welcome to Day 20! Over the past 19 days, we have built a fully functional, highly optimized, vectorized columnar analytical database engine from scratch in C11. We have zero-copy memory mapping, SIMD vectorized filtering, high-cardinality dictionaries, free-text search, disk-spilling hash aggregations, and background compaction.

But up to this point, interacting with `simpledb` has required writing raw C unit tests and linking directly against the internal engine APIs. A database needs an accessible query language. Today, we build the query front-end: a **Lexer**, a **Recursive Descent Parser**, and an **Abstract Syntax Tree (AST)** for SQL queries.

## The Goal: A Minimal Analytical SQL Dialect

We want to support an expressive analytical subset of SQL, combined with the array-oriented brevity inspired by K/q (from our Day 4 APL inspirations).
Our parser will understand queries like:

```sql
SELECT user_id, sum(amount) FROM events WHERE amount > 100 GROUP BY user_id ORDER BY user_id DESC;
INSERT INTO events (user_id, status) VALUES (42, 'SUCCESS');
INSERT INTO events FROM 'data.json';
```

We build this in two core stages:
1. **The Lexer (`sql_lexer_t`)**: Scans the raw SQL query string and emits strongly typed tokens (e.g., `SELECT`, `INSERT`, `IDENTIFIER("user_id")`, `COMMA`, `NUMBER(100)`, `STRING('SUCCESS')`).
2. **The Parser (`sql_parser_t`)**: An LL(1) recursive descent parser with 1-token lookahead that consumes tokens and builds a clean `ast_query_t` representing projections, aggregate functions (`SUM`, `COUNT`, `MAX`, `MIN`), aliases (`AS`), filter predicates (`WHERE`), and `GROUP BY` / `ORDER BY` clauses.

In **Day 21**, we will take this parser and connect it to an interactive terminal REPL shell, dynamic schema inference, streaming NDJSON ingestion with `yyjson`, and vectorized query execution!

## The Theory: Lexers vs. Parsers

When building compilers or databases, transforming a string of text into executable machine actions is universally split into two distinct phases:

### 1. Lexical Analysis (The Lexer)
The Lexer (or Scanner) acts as a low-level text categorizer. It iterates over the raw string character by character and groups them into logical "words" called **Tokens**. 
* **What it does:** It strips out meaningless characters (like spaces, tabs, and newlines) and identifies primitive types. For example, it sees `100` and classifies it as `TOKEN_NUMBER`, or sees `SELECT` and classifies it as `TOKEN_SELECT`.
* **What it doesn't do:** The Lexer does not care about grammar. If you type `FROM 100 SELECT WHERE`, the Lexer will happily return valid tokens for all of those words, even though it's complete gibberish in SQL.
* **C Linkage Question: Should `sql_lexer_next_token` be `static`?**
  In C, marking a function `static` gives it internal linkage, making it invisible outside that `.c` file. If the lexer is strictly an internal helper that nobody outside `src/sql_parser.c` ever calls, it *could* be `static`. 
  However, by declaring `sql_lexer_next_token` in `include/simpledb/sql_parser.h` (external linkage, non-`static`), we gain a huge advantage: **testability**. We can write isolated unit tests that verify tokenization of edge cases (e.g. string literals, numbers, multi-character operators) directly, without having to construct full valid SQL queries through the parser! Note that if a function is declared in a public `.h` header, defining it as `static` in `.c` will cause compiler/linker conflicts.

### Lexer Theory: Regular Grammars, Automata & The "Optimal" Approach

To truly understand what makes a lexer optimal, we must examine the formal theory:

#### 1. The Chomsky Hierarchy: Why Lexing is Separated from Parsing
In formal language theory (Noam Chomsky, 1956), formal grammars are classified into four tiers:
- **Type 3 (Regular Grammars):** Tokens (keywords, numbers, identifiers, operators). They can be recognized by **Finite State Automata** with zero auxiliary memory (no stack needed).
- **Type 2 (Context-Free Grammars):** Nested syntax (expressions like `((a + b) * c)` or SQL subqueries `SELECT * FROM (SELECT ...)`). They require a **Pushdown Automaton** (a state machine equipped with a stack).
- **Type 1 (Context-Sensitive) & Type 0 (Unrestricted Turing-Complete):** Semantic validation, type checking, etc.

Because tokens are strictly **Regular Languages**, separating the Lexer allows us to run an ultra-fast `O(N)` linear scan over the raw characters with zero stack allocations, converting raw bytes into integer enums before the heavier syntax parser ever runs.

#### 2. The Theoretical Engine: Deterministic Finite Automata (DFA)
Mathematically, any regular expression can be converted into a Non-Deterministic Finite Automaton (NFA) via **Thompson's Construction**, and then into a **Deterministic Finite Automaton (DFA)** via **Powerset Construction** (Subset Construction).
A DFA is a 5-tuple:
`M = (Q, Σ, δ, q₀, F)`
- Q: Finite set of states.
- Σ: Alphabet (input bytes `0..255`).
- `δ: Q × Σ -> Q`: Transition function.
- `q₀ ∈ Q`: Start state.
- `F ⊆ Q`: Set of accepting states (token emitted).

In a DFA, there is **exactly one** transition for any given (state, input byte) pair. This mathematically guarantees that scanning N characters takes strictly `O(N)` operations with **zero backtracking**.

#### 3. The Maximal Munch Rule (Longest Match)
When scanning `>` vs `>=` or `SELECT` vs an identifier like `selection`, two regular expressions may match a prefix. Lexers universally implement **Maximal Munch**: the lexer greedily consumes characters as long as a valid transition exists, matching the longest possible prefix.

#### 4. The Engineering Trade-off: What is the "Optimal" Approach?
There are 4 main architectural paradigms to implement a lexer in C:

| Approach | How it works | Throughput | Pros / Cons |
| :--- | :--- | :--- | :--- |
| **1. Table-Driven DFA** (Lex, Flex) | `state = table[state][char]` in a loop | ~200 - 500 MB/s | **Pros:** Clean, mathematically pure, generated from regex files.<br>**Cons:** Table memory indirection defeats CPU branch prediction; L1/L2 cache misses on large token tables. |
| **2. Direct-Coded DFA** (`re2c`, Ragel) | Emits hardcoded C `switch` and `goto` labels for each state | ~1.5 - 3.0 GB/s | **Pros:** Highly optimal on scalar CPUs; hardware branch predictors learn the jump targets directly.<br>**Cons:** Code bloat (thousands of generated lines of C); hard to maintain by hand. |
| **3. Hand-Written Dispatch** (SQLite, PostgreSQL, Clang, DuckDB, `simpledb`) | Direct character classification (`isspace`, `isalpha`, `switch(c)`) | ~800 MB/s - 1.5 GB/s | **Pros:** Zero external build dependencies, crystal-clear error messages, tiny binary footprint (<200 LOC), perfectly adequate since query text is small.<br>**Cons:** Requires manual keyword hashing/trie matching. |
| **4. SIMD Structural Indexing** (`simdjson`, Hyperscan) | Uses AVX2/Neon 256-bit bitmasks to find structural characters in parallel | **> 4.0 GB/s** | **Pros:** Reaches RAM memory bus bandwidth limits.<br>**Cons:** Extremely complex, requires AVX2/AVX-512/Neon intrinsics; overkill for short SQL queries, but essential for multi-gigabyte JSON files (as seen in `jsonbench.com`). |

For a SQL database query engine, queries are typically small (a few hundred bytes), making **Hand-Written Dispatch (Approach 3)** the undisputed industry standard across SQLite, DuckDB, and Clang.

#### Deep Dive: Lex & Flex (The Classic Generators)

**`lex`** was developed in 1975 by Mike Lesk and Eric Schmidt at Bell Labs as part of the original Unix compiler toolchain. **`flex`** ("Fast Lexical Analyzer Generator") was later written by Vern Paxson in 1987 as a faster, open-source replacement.

##### 1. How Lex/Flex Works
Instead of writing C code, you write a declarative specification file with a `.l` extension consisting of regular expressions paired with C actions:

```lex
%{
#include "sql_parser.h"
%}

%%
[ \t\r\n]+             { /* Skip whitespace */ }
"SELECT"               { return TOKEN_SELECT; }
"FROM"                 { return TOKEN_FROM; }
"WHERE"                { return TOKEN_WHERE; }
[0-9]+                 { return TOKEN_NUMBER; }
[a-zA-Z_][a-zA-Z0-9_]* { return TOKEN_IDENTIFIER; }
.                      { return TOKEN_ERROR; }
%%
```

When you build your project, the build system invokes the `flex` CLI tool:
1. **Regex -> NFA:** Flex parses your regular expressions and applies Thompson's Construction to build a single Non-Deterministic Finite Automaton.
2. **NFA -> DFA:** It converts the NFA into a Deterministic Finite Automaton using the Powerset/Subset Construction algorithm.
3. **Minimization:** It optimizes the number of states using Hopcroft's DFA minimization algorithm.
4. **Code Generation:** It emits a standalone C file, `lex.yy.c` (often 1,500 to 3,000 lines of dense C code), containing compressed 2D state transition tables:
   ```c
   /* Generated table lookup inside yylex() */
   int yy_next_state = yy_nxt[yy_base[state] + byte];
   ```

##### 2. Why Lex/Flex was Revolutionary
In the 1970s and 1980s, hand-writing state machines was considered tedious and prone to subtle edge-case bugs. Flex guaranteed mathematical correctness: it proved your token set had no ambiguities and guaranteed strict `O(N)` scanning with the Maximal Munch rule automatically. Paired with `yacc` / `bison`, it defined the standard Unix toolchain.

##### 3. Why Modern Engines (SQLite, DuckDB, Clang) Abandoned Flex
Despite its theoretical elegance, almost all modern compilers and database engines have moved away from Flex:
1. **External Build Toolchain Dependency:** Your build now requires the `flex` executable installed on every developer machine, CI runner, and cross-compilation target. This complicates `CMakeLists.txt` (`find_package(FLEX)`).
2. **Poor CPU Cache & Branch Prediction Performance:** The table-driven loop (`yy_nxt[...]`) forces repeated 2D array lookups across large tables in memory. Modern superscalar CPUs stall on indirect memory loads and cannot predict array lookups, whereas hardcoded `switch(c)` branches are predicted at near 100% accuracy by CPU branch predictor hardware.
3. **Reentrancy and Thread Safety:** Classic Flex relies on global state variables (`yytext`, `yyleng`, `yyin`). Making Flex reentrant requires `%option reentrant`, which introduces cumbersome pointer wrappers (`yyscan_t`) that complicate thread-safe concurrent query parsing.
4. **Debugging and Error Diagnostics:** If a user writes an invalid SQL query, you want to show helpful error messages (e.g., *"Did you forget a comma after user_id at line 1, col 14?"*). With Flex's generated table code, extracting rich context, backtracking cleanly, or handling dialect quirks is notoriously difficult.
5. **Code Bloat:** For our ~15 SQL tokens, a hand-written C lexer takes **~100 lines of clean, readable C11 code**. Flex would generate a 2,500-line `lex.yy.c` labyrinth of legacy compatibility macros.

### 2. Syntax Analysis (The Parser)
The Parser consumes the stream of Tokens produced by the Lexer and applies **Grammar Rules** to build an **Abstract Syntax Tree (AST)**. 
* **What it does:** It understands the structural relationship between tokens. For a SQL `SELECT` statement, the parser knows the grammar demands `SELECT <exprs> FROM <table> [WHERE <predicates>]`. It checks the sequence of tokens, validates that they appear in the correct order, and arranges them into a hierarchical tree (the AST) that the physical execution engine can easily evaluate.
* **Top-Down Parsing:** We will use a technique called **Recursive Descent**. It starts at the top-level grammar rule (e.g., `parse_query()`) and recursively calls smaller functions based on the current token (e.g., if the token is `SELECT`, it calls `parse_select()`; if it's `INSERT`, it calls `parse_insert()`).

### Further Reading: Compilers & Parsers
If you find building the Lexer and Parser fascinating, writing interpreters and compilers is one of the most rewarding areas of computer science. Highly recommended resources include:
1. **[Crafting Interpreters](https://craftinginterpreters.com/) by Bob Nystrom:** The absolute gold standard modern book. It walks you through building a tree-walking interpreter in Java, and then a blazing-fast bytecode Virtual Machine in C. It explains the exact hand-written C Lexer and Recursive Descent parser implementation we are building today, step-by-step, line-by-line.
2. **Writing An Interpreter In Go by Thorsten Ball:** A very practical, code-first book that teaches Pratt Parsing (a technique excellent for parsing complex mathematical expressions with operator precedence).
3. **The "Dragon Book" (Compilers: Principles, Techniques, and Tools):** The heavy, classic academic textbook. It is incredibly dense and focuses heavily on parser generators (like YACC/Bison) and automata theory rather than hand-written parsers, but it is the definitive theoretical reference.

## 1. The Headers

Here are the headers for our parser and REPL.

### `include/simpledb/sql_parser.h`

```c
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Token types for the lexer
typedef enum {
    TOKEN_EOF = 0,
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_GROUP,
    TOKEN_ORDER,
    TOKEN_BY,
    TOKEN_ASC,
    TOKEN_DESC,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_COMMA,
    TOKEN_EQUALS,
    TOKEN_GREATER,
    TOKEN_LESS,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_ERROR
} token_type_t;

// A single token
typedef struct {
    token_type_t type;
    char text[64];
    int length;
} sql_token_t;

// The Lexer state
typedef struct {
    const char* input;
    size_t pos;
    size_t len;
} sql_lexer_t;

sql_lexer_t sql_lexer_create(const char* query);
sql_token_t sql_lexer_next_token(sql_lexer_t* lexer);

// Abstract Syntax Tree (AST) Nodes
typedef enum {
    AST_AGG_SUM,
    AST_AGG_MAX,
    AST_AGG_MIN,
    AST_AGG_COUNT,
    AST_PROJECTION
} ast_expr_type_t;

typedef struct {
    ast_expr_type_t type;
    char column_name[64];
    char alias[64];
} ast_expr_t;

typedef enum {
    OP_EQUALS,
    OP_GREATER_THAN,
    OP_LESS_THAN
} ast_op_t;

typedef struct {
    char column_name[64];
    ast_op_t op;
    char string_val[64];
    int32_t int_val;
    bool is_string;
} ast_predicate_t;

typedef enum {
    STMT_SELECT,
    STMT_INSERT
} ast_stmt_type_t;

typedef struct {
    int32_t int_val;
    char string_val[64];
    bool is_string;
} ast_value_t;

typedef struct {
    ast_stmt_type_t stmt_type;
    char table_name[64];
    
    // For SELECT
    ast_expr_t select_exprs[16];
    size_t num_select_exprs;
    ast_predicate_t predicates[16];
    size_t num_predicates;
    char group_by_col[64];
    bool has_group_by;
    char order_by_col[64];
    bool has_order_by;
    bool order_desc;

    // For INSERT (e.g., INSERT INTO table (col1, col2) VALUES (1, 'a'))
    char insert_cols[16][64];
    size_t num_insert_cols;
    ast_value_t insert_vals[16];
    size_t num_insert_vals;
    
    // For bulk JSON insertion (e.g., INSERT INTO table FROM 'data.json')
    bool is_bulk_insert;
    char from_file_path[256];
} ast_query_t;

// The Parser state
typedef struct {
    sql_lexer_t lexer;
    sql_token_t current_token;
    sql_token_t peek_token;
} sql_parser_t;

sql_parser_t sql_parser_create(const char* query);
bool sql_parser_parse(sql_parser_t* parser, ast_query_t* out_query);
```

## 2. Mocking and Testing

Let's verify that the parser correctly builds the AST for both `SELECT` and `INSERT` queries.

### `tests/test_db.c` (`test_sql_parser`)

#### C Quirk: Why `#undef NDEBUG` Before `#include <assert.h>` in Unit Tests?

In standard C (`<assert.h>`), the `assert(expr)` macro is controlled by the preprocessor macro `NDEBUG` ("**N**o **DEBUG**"):
```c
#ifdef NDEBUG
    #define assert(condition) ((void)0)
#else
    #define assert(condition) /* check condition and abort() if false */
#endif
```
1. **What CMake `Release` Mode Does:** When you configure CMake with `-DCMAKE_BUILD_TYPE=Release` (which is how our `build/` directory is configured for benchmarking), CMake automatically passes `-O3 -DNDEBUG` to the compiler.
2. **The Silent Test Trap:** With `-DNDEBUG` active, **every single `assert(...)` in your test file is erased by the preprocessor** and replaced with `((void)0)`! That means:
   - Your unit tests don't actually check anything—even if `ast.int_val` is completely wrong, the test still prints `"passed!"`.
   - Worse, if you write `bool ok = sql_parser_parse(&parser, &ast); assert(ok);` and put side-effecting calls inside `assert(do_something() == 0)`, the compiler either warns `unused variable 'ok'` or skips calling `do_something()` entirely!
3. **Why `<assert.h>` Has No `#pragma once`:** Uniquely in the C11 standard (§7.2), `<assert.h>` is the **only** standard library header that is *forbidden* from having include guards (`#pragma once` / `#ifndef`). Placing `#undef NDEBUG` right before `#include <assert.h>` at the top of `tests/test_db.c` forces `assert()` back on across the entire test suite—even in an `-O3 -DNDEBUG` Release build! (In fact, moving `#undef NDEBUG` to the top of `tests/test_db.c` immediately caught an older Day 14 assertion in `test_table_flush_e2e` that was still checking `./user_id.bin` instead of Day 19's ClickHouse-style `./all_1_1_0/user_id.bin` directory—which had been silently skipped when `NDEBUG` was defined!)
4. **Cleaning Up Part Directories (`remove()` in `<stdio.h>`):** On POSIX systems, standard C's `remove(path)` calls `unlink()` for files and `rmdir()` for directories. However, `rmdir()` fails with `ENOTEMPTY` if any files still exist inside the directory! Therefore, tests that create disk parts (`test_table_flush_e2e`, `test_grace_hash_agg`, `test_part_merger`) must `remove()` the inner `.bin`/`.idx`/`.dict` files *before* calling `remove()` on the parent part directory (`./all_1_1_0`, `events_merge_test`, `test_grace_agg_tmp`).

```c
// At the top of tests/test_db.c:
#undef NDEBUG
#include <assert.h>

// ...

static void test_sql_parser(void) {
    // 1. Test SELECT with projection, aggregate, alias, WHERE, GROUP BY, and ORDER BY DESC
    {
        const char* query = "SELECT user_id, sum(amount) AS total FROM events WHERE amount > 100 GROUP BY user_id ORDER BY total DESC;";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};

        bool ok = sql_parser_parse(&parser, &ast);
        assert(ok);
        (void)ok;
        assert(ast.stmt_type == STMT_SELECT);

        assert(strcmp(ast.table_name, "events") == 0);
        assert(ast.num_select_exprs == 2);
        assert(ast.select_exprs[0].type == AST_PROJECTION);
        assert(strcmp(ast.select_exprs[0].column_name, "user_id") == 0);

        assert(ast.select_exprs[1].type == AST_AGG_SUM);
        assert(strcmp(ast.select_exprs[1].column_name, "amount") == 0);
        assert(strcmp(ast.select_exprs[1].alias, "total") == 0);

        assert(ast.num_predicates == 1);
        assert(strcmp(ast.predicates[0].column_name, "amount") == 0);
        assert(ast.predicates[0].op == OP_GREATER_THAN);
        assert(ast.predicates[0].int_val == 100);

        assert(ast.has_group_by);
        assert(strcmp(ast.group_by_col, "user_id") == 0);

        assert(ast.has_order_by);
        assert(strcmp(ast.order_by_col, "total") == 0);
        assert(ast.order_desc == true);
    }

    // 2. Test Single-Row INSERT ... VALUES
    {
        const char* query = "INSERT INTO events (user_id, status) VALUES (42, 'SUCCESS');";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};

        bool ok = sql_parser_parse(&parser, &ast);
        assert(ok);
        (void)ok;
        assert(ast.stmt_type == STMT_INSERT);

        assert(strcmp(ast.table_name, "events") == 0);

        assert(ast.num_insert_cols == 2);
        assert(strcmp(ast.insert_cols[0], "user_id") == 0);
        assert(strcmp(ast.insert_cols[1], "status") == 0);

        assert(ast.num_insert_vals == 2);
        assert(ast.insert_vals[0].is_string == false);
        assert(ast.insert_vals[0].int_val == 42);

        assert(ast.insert_vals[1].is_string == true);
        assert(strcmp(ast.insert_vals[1].string_val, "SUCCESS") == 0);
    }

    // 3. Test Bulk JSON INSERT ... FROM
    {
        const char* query = "INSERT INTO events FROM 'events_data.json';";
        sql_parser_t parser = sql_parser_create(query);
        ast_query_t ast = {0};

        bool ok = sql_parser_parse(&parser, &ast);
        assert(ok);
        (void)ok;
        assert(ast.stmt_type == STMT_INSERT);

        assert(strcmp(ast.table_name, "events") == 0);
        assert(ast.is_bulk_insert == true);
        assert(strcmp(ast.from_file_path, "events_data.json") == 0);
    }

    printf("Day 20 SQL Parser tests passed (SELECT, INSERT, & Bulk JSON)!\n");
}
```

## 3. Implementation Hints & Day 20 Roadmap

To tackle Day 20 efficiently, we break the day into 4 clear milestones:

### The Day 20 Roadmap
1. **Milestone 1: The Lexer State Machine (`src/sql_parser.c`)**
   - Implement `sql_lexer_create()` and `sql_lexer_next_token()`.
   - Defensive initialization: guard against `NULL` query strings before invoking `strlen()`.
   - Skip whitespace (`isspace()`).
   - Match punctuation tokens: `,`, `(`, `)`, `=`, `>`, `<`.
   - Match string literals (`'...'`) and number literals (`0-9+`).
   - Match alphanumeric words: check for SQL keywords (`SELECT`, `FROM`, `WHERE`, `GROUP`, `BY`, `INSERT`, `INTO`, `VALUES`), otherwise classify as `TOKEN_IDENTIFIER`.
2. **Milestone 2: The Recursive Descent Parser (`src/sql_parser.c`)**
   - Implement lookahead helpers: `parser_advance()`, `parser_match()`, and `parser_expect()`.
   - Implement statement routing: if `TOKEN_SELECT` -> `parse_select()`; if `TOKEN_INSERT` -> `parse_insert()`.
   - Populate the `ast_query_t` output struct.
3. **Milestone 3: Verify with Unit Tests (`tests/test_day20.c`)**
   - Compile and run the test suite to guarantee that both `SELECT` and `INSERT` (single-row & bulk JSON) produce exact ASTs.
4. **Milestone 4: The Interactive REPL Shell (`src/repl.c`)**
   - Wire `sql_parser_parse()` to our physical query execution pipeline (Day 18) and table ingestion (Day 14).
   - Render query execution results in an ASCII table and print timings in milliseconds.

### Anatomy of Option 3: The Hand-Written Character Dispatch Loop

In **Option 3 (Hand-Written Dispatch)**, `sql_lexer_next_token(sql_lexer_t *lexer)` is organized into four sequential phases:

```
[Input Query] ---> (Phase 1: Skip Whitespace)
                         |
                         v
                   (Phase 2: Check EOF)
                         |
                         v
                   (Phase 3: First-Character Inspection: `c`)
                   /         |              |             \
             Punctuation   Quote `'`      Digit         Letter / `_`
                  |          |              |              |
               Phase 4A   Phase 4B       Phase 4C       Phase 4D
              (Single-Char) (String)      (Number)     (Identifier / Keyword)
```

1. **Phase 1: Whitespace Trimming**
   Skip spaces, tabs, and newlines using `<ctype.h>`:
   ```c
   while (lexer->pos < lexer->len && isspace((unsigned char)lexer->input[lexer->pos])) {
       lexer->pos++;
   }
   ```
2. **Phase 2: EOF Guard**
   If `lexer->pos >= lexer->len` (or if `lexer->input[lexer->pos] == ';'`), advance past `;` if present and return `(sql_token_t){.type = TOKEN_EOF}`.
3. **Phase 3 & 4: First-Character Dispatch (`char c = lexer->input[lexer->pos]`)**
   Inspect `c` and branch into one of 4 parsing paths:
   - **4A. Punctuation (`switch (c)`):**
     For single-character tokens (`,`, `(`, `)`, `=`, `>`, `<`), advance `lexer->pos++` and return the corresponding `sql_token_t`:
     ```c
     switch (c) {
         case ',': lexer->pos++; return (sql_token_t){.type = TOKEN_COMMA,   .text = ",", .length = 1};
         case '(': lexer->pos++; return (sql_token_t){.type = TOKEN_LPAREN,  .text = "(", .length = 1};
         case ')': lexer->pos++; return (sql_token_t){.type = TOKEN_RPAREN,  .text = ")", .length = 1};
         case '=': lexer->pos++; return (sql_token_t){.type = TOKEN_EQUALS,  .text = "=", .length = 1};
         case '>': lexer->pos++; return (sql_token_t){.type = TOKEN_GREATER, .text = ">", .length = 1};
         case '<': lexer->pos++; return (sql_token_t){.type = TOKEN_LESS,    .text = "<", .length = 1};
         case ';': lexer->pos++; return (sql_token_t){.type = TOKEN_EOF,     .text = ";", .length = 1};
     }
     ```
   - **4B. String Literals (`c == '\''`):**
     Advance past the opening quote (`lexer->pos++`). Record `size_t start = lexer->pos;`. Scan forward while `lexer->pos < lexer->len && lexer->input[lexer->pos] != '\''`.
     - **Unterminated String Check:** If `lexer->pos >= lexer->len`, the string literal had no closing quote—return `TOKEN_ERROR`!
     - Otherwise, skip the closing quote (`lexer->pos++`), check `if (len >= sizeof(tok.text)) return TOKEN_ERROR`, `memcpy(tok.text, &lexer->input[start], len)`, null-terminate `tok.text[len] = '\0'`, and return `TOKEN_STRING`.
   - **4C. Number Literals (`isdigit((unsigned char)c)`):**
     Record `size_t start = lexer->pos;`. Since the outer `if` *already* verified that the character at `start` is a digit, we can immediately advance `lexer->pos++` (or use a `do-while` loop) so we don't redundantly re-check the first character! Then advance while `lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos])`:
     ```c
     if (isdigit((unsigned char)c)) {
         size_t start = lexer->pos;
         do {
             lexer->pos++;
         } while (lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos]));

         // Optional: Support floating-point literals (e.g., 3.14)
         if (lexer->pos + 1 < lexer->len &&
             lexer->input[lexer->pos] == '.' &&
             isdigit((unsigned char)lexer->input[lexer->pos + 1])) {
             do {
                 lexer->pos++;
             } while (lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos]));
         }

         size_t len = lexer->pos - start;
         sql_token_t tok = {.type = TOKEN_NUMBER, .length = (int)len};
         if (len >= sizeof(tok.text)) {
             return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
         }
         memcpy(tok.text, &lexer->input[start], len);
         tok.text[len] = '\0';
         return tok;
     }
     ```
     * **C Quirk: Why Does `int isdigit(int c)` Expect an `int`, Yet Require `(unsigned char)c`?**
       1. **Why the signature takes `int`:** `isdigit(int c)` was designed to accept the return value of `fgetc()` / `getchar()`, which returns any valid byte (`0..255`) **or** `EOF` (`-1`). Since `0..255` plus `-1` is **257 distinct values**, it cannot fit in a 1-byte `char` (which only holds 256 states), so the parameter type had to be `int`.
       2. **Sign-Extension vs. Zero-Extension:** On x86 and ARM64 macOS, plain `char` is **signed** (`-128..127`). If `c` holds byte `0xE2` (which is `-30` as a signed `char`), passing `c` or `(int)c` performs **sign-extension** (`0xE2` -> `int -30`). Inside libc, `isdigit(c)` indexes directly into a lookup array `(_ctype_ + 1)[c]`—so passing `-30` reads out-of-bounds memory 29 bytes *before* the table!
       3. **What `(unsigned char)c` does:** Casting `(unsigned char)c` first reinterprets `0xE2` as `+226`, and C then automatically **zero-extends** that `unsigned char` to `int +226` to match the `int` parameter signature safely.
     * **What about Floats?** Even though our Day 20 AST (`ast_predicate_t` and `ast_value_t`) currently evaluates `int32_t int_val` for our integer/dictionary table columns, allowing the lexer to consume an optional `.` followed by digits (`3.14`) ensures the tokenizer emits a single `TOKEN_NUMBER("3.14")` instead of failing with `TOKEN_ERROR` on the decimal point! To support float predicates end-to-end in the AST, you would add `double float_val; bool is_float;` to `ast_predicate_t` and `ast_value_t` and parse with `strtod()`.
   - **4D. Words: Keywords & Identifiers (`isalpha((unsigned char)c) || c == '_'`):**
     Record `size_t start = lexer->pos;`. Just like with numbers, we already know the first character is valid, so advance `lexer->pos++` first, then loop while `isalnum((unsigned char)c) || c == '_' || c == '.'` (supporting dotted identifiers like `table.column`). Copy the slice into `tok.text` and classify keywords with `strcasecmp` (from `<strings.h>`):
     ```c
     if (isalpha((unsigned char)c) || c == '_') {
         size_t start = lexer->pos;
         do {
             lexer->pos++;
         } while (lexer->pos < lexer->len &&
                  (isalnum((unsigned char)lexer->input[lexer->pos]) ||
                   lexer->input[lexer->pos] == '_' ||
                   lexer->input[lexer->pos] == '.'));
         size_t len = lexer->pos - start;
         sql_token_t tok = {.type = TOKEN_IDENTIFIER, .length = (int)len};
         if (len >= sizeof(tok.text)) {
             return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
         }
         memcpy(tok.text, &lexer->input[start], len);
         tok.text[len] = '\0';

         if (strcasecmp(tok.text, "SELECT") == 0) tok.type = TOKEN_SELECT;
         else if (strcasecmp(tok.text, "FROM") == 0) tok.type = TOKEN_FROM;
         else if (strcasecmp(tok.text, "WHERE") == 0) tok.type = TOKEN_WHERE;
         else if (strcasecmp(tok.text, "GROUP") == 0) tok.type = TOKEN_GROUP;
         else if (strcasecmp(tok.text, "BY") == 0) tok.type = TOKEN_BY;
         else if (strcasecmp(tok.text, "INSERT") == 0) tok.type = TOKEN_INSERT;
         else if (strcasecmp(tok.text, "INTO") == 0) tok.type = TOKEN_INTO;
         else if (strcasecmp(tok.text, "VALUES") == 0) tok.type = TOKEN_VALUES;
         return tok;
     }
     ```
     * **Why check `c == '_'`?** In `<ctype.h>`, `isalpha()` and `isalnum()` only match ASCII letters (`a-z`, `A-Z`) and digits (`0-9`)—they return **false (`0`)** for the underscore character `'_'`. In SQL, snake_case column names like `user_id` and virtual system columns like ClickHouse's `_part` or `_id` are everywhere. Without explicitly allowing `'_'` in the `do-while` condition, the lexer would stop at the underscore in `user_id`, emit `"user"`, and then fail with `TOKEN_ERROR` on `'_'`! Allowing `c == '_'` in the outer `if` also supports identifiers that begin with an underscore (like `_id`).
   - **Fallback (`lexer->pos++` before `TOKEN_ERROR`):**
     Why increment `lexer->pos++` before returning `TOKEN_ERROR`? In compiler design, this enforces the **Forward Progress Invariant**: every call to `sql_lexer_next_token()` before `TOKEN_EOF` must strictly advance `lexer->pos` by at least 1 byte. If the lexer encountered an unrecognized character (like `@`) and returned `TOKEN_ERROR` *without* advancing `lexer->pos`, any caller looping until `TOKEN_EOF` (or `sql_parser_create` calling `next_token` twice) would get permanently stuck inspecting the exact same invalid character in an **infinite loop**.

#### C History Quirk: `<string.h>` vs. `<strings.h>` — Why Do Both Exist?

It looks bizarre to include both `<string.h>` and `<strings.h>` in the same file. This is a fossil from the **1980s Unix Wars** between AT&T Bell Labs (System V) and UC Berkeley (BSD):
- **`<string.h>` (singular):** Created by AT&T System V Unix for `strlen`, `strcmp`, `strcpy`, `memcpy`, and `memset`. When ANSI C (C89) standardized the C language, it adopted AT&T's `<string.h>`.
- **`<strings.h>` (plural):** Created by BSD Unix for `bcopy`, `bzero`, `index`, and **`strcasecmp` / `strncasecmp`**. Because case-folding rules vary across human languages, ANSI/ISO C refused to put `strcasecmp` into standard `<string.h>`. Instead, the **POSIX** standard preserved `<strings.h>` specifically for `strcasecmp` and `strncasecmp`.
- **Pure ISO C11 Alternative:** If you ever compile on a non-POSIX platform (like MSVC on Windows, which calls it `_stricmp`), you can avoid `<strings.h>` with a 5-line ASCII helper using `tolower((unsigned char)*a)` from `<ctype.h>`.

#### C Memory Safety Note: Fail-Fast `TOKEN_ERROR` vs. Silent Truncation

In `include/simpledb/sql_parser.h`, a token stores its text inside a fixed-size 64-byte stack array:
```c
typedef struct {
    token_type_t type;
    char text[64];
    int length;
} sql_token_t;
```
1. **Why 63 Payload Bytes?** In C, strings passed to functions like `strcmp()`, `strcasecmp()`, or `atoi()` must be null-terminated (`'\0'`). A 64-byte array (`indices 0..63`) can hold at most **63 payload characters**, reserving the final byte (`tok.text[63]`) for `'\0'`. Without a bounds check, an identifier or string literal of 64+ characters would write past the end of `tok.text`—causing a classic **stack buffer overflow**.
2. **Fail-Fast (`TOKEN_ERROR`) Beat Silent Truncation:** Rather than silently clamping `len` (which could corrupt a file path or column name without warning), database engines should **fail fast** by returning `TOKEN_ERROR` whenever a token exceeds the buffer capacity:
   ```c
   if (len >= sizeof(tok.text)) {
       return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
   }
   ```
3. **Production Alternative (Zero-Copy String Views):** Notice that our Day 2 `slice_t` concept applies to lexers too! Production compilers (like Clang or `clox` in *Crafting Interpreters*) avoid `char text[64]` and `memcpy` entirely by storing a zero-copy pointer directly into the source query string (`const char* start; size_t length;`). We use `char text[64]` here only for convenience so `tok.text` is immediately null-terminated for `strcmp` and `atoi`.

### Scanning Techniques in C: `isspace()` vs. `strspn()` vs. `memchr()`

When optimizing lexers, engineers often look to the C standard library (`<string.h>`):

1. **Why `memchr()` cannot skip whitespace:**
   `void *memchr(const void *s, int c, size_t n);` scans memory for a **single, specific target byte** `c`. Whitespace in SQL is a *set* of characters (`' '`, `'\t'`, `'\r'`, `'\n'`). More fundamentally, skipping whitespace requires finding the first character that is **NOT** in that set (an inverted search). `memchr()` can only find matches of one byte, not non-matches of a set.

2. **Where `memchr()` shines in Lexers:**
   `memchr()` is extremely fast (heavily SIMD-vectorized in glibc/macOS libSystem) for finding closing single delimiters! For example, when scanning a string literal `'hello world'`, finding the closing quote `'` with `memchr(&input[pos + 1], '\'', remaining_len)` scans at full memory bandwidth (30+ GB/s) instead of character-by-character loops.

3. **Skipping Whitespace: `strspn()` vs `while (isspace())`:**
   - **`strspn(s, " \t\r\n")`**: Computes the length of the initial segment consisting entirely of characters from the reject set. This skips all whitespace in one call.
   - **`while (pos < len && isspace((unsigned char)s[pos])) pos++;`**: Between SQL tokens, whitespace is typically just 1 single space character. The branch predictor and L1 cache handle this scalar loop in 1-2 CPU cycles, with zero libc call overhead. Both approaches are valid, but the simple `isspace` loop is standard and self-contained.

### Is Tokenization Just Splitting by Whitespace?

A common intuition when starting out with lexers is: *"Can't we just split the query on whitespace (like `strtok` or Python's `.split()`)?"*

The answer is **no**, for three major reasons:

1. **Punctuation glued to identifiers:**
   In SQL, users write `SELECT user_id, amount`. If you split on whitespace, the first word is `"user_id,"` with the comma attached. The lexer must recognize `"user_id"` as `TOKEN_IDENTIFIER` and `","` as `TOKEN_COMMA`.
2. **Expressions with zero whitespace:**
   A valid SQL predicate is `WHERE amount>100` or an aggregate like `sum(amount)`. Splitting by whitespace would see `"amount>100"` as a single token. A lexer breaks this down into three tokens: `TOKEN_IDENTIFIER("amount")`, `TOKEN_GREATER`, `TOKEN_NUMBER(100)`.
3. **Whitespace inside literals:**
   In string literals like `'SUCCESSFUL EVENT'`, whitespace is part of the value, not a delimiter. The lexer must preserve the whole string within single quotes as one `TOKEN_STRING`.

Therefore, after skipping leading whitespace, `sql_lexer_next_token` routes into **4 distinct categories**:
1. **Single-character delimiters:** `,`, `(`, `)`, `=`, `>`, `<` -> return immediately.
2. **String literals:** starts with `'` -> scan until closing `'`.
3. **Number literals:** starts with `isdigit()` -> scan all digits.
4. **Words (Keywords & Identifiers):** starts with `isalpha()` or `_` -> scan characters, then check if the word is a known keyword (`SELECT`, `FROM`, etc.) or a column/table name (`TOKEN_IDENTIFIER`).

### Parser Initialization & LL(1) Lookahead (`current_token` vs. `peek_token`)

In compiler theory, this pattern is formally called an **LL(1) Parser** with a **2-token sliding window**:
- **First `L` (Left-to-right):** Scans the token stream from left to right.
- **Second `L` (Leftmost derivation):** Top-down recursive descent through the grammar rules.
- **`(1)` (1-Token Lookahead):** Uses 1 token of lookahead (`peek_token`) to decide which grammar branch to take *without* backtracking.

Our parser struct holds two tokens at all times:
```c
typedef struct {
    sql_lexer_t lexer;
    sql_token_t current_token; // The token we are inspecting right now
    sql_token_t peek_token;    // The NEXT token waiting in line (1-token lookahead)
} sql_parser_t;
```

#### Visual Trace: How `current_token` and `peek_token` Slide Across a Query
Imagine the Lexer turns `SELECT user_id, sum(amount) FROM events;` into a conveyor belt of tokens:
```
[SELECT] -> [user_id] -> [,] -> [sum] -> [(] -> [amount] -> [)] -> [FROM] -> [events] -> [EOF]
```
Think of `current_token` and `peek_token` as a **2-slot window** moving from left to right:

| Step | Action | `current_token` (Now) | `peek_token` (Next) | Why `peek_token` Matters Here |
| :--- | :--- | :--- | :--- | :--- |
| **0** | `sql_parser_create()` ("Prime the Pump") | `SELECT` | `user_id` | `current_token` is `SELECT`, so we call `parse_select()`. |
| **1** | `parser_advance()` (consume `SELECT`) | `user_id` | `,` | `current_token` is an identifier (`user_id`). Is it a column or a function call? We peek at `peek_token`: it's `,` (not `(`), so `user_id` is a plain column (`AST_PROJECTION`)! |
| **2** | `parser_advance()` twice (consume `user_id`, `,`) | `sum` | `(` | `current_token` is an identifier (`sum`). We peek at `peek_token`: it **IS** `(` (`TOKEN_LPAREN`)! That tells us `sum` is an aggregate function (`AST_AGG_SUM`)! |
| **3** | `parser_advance()` twice (consume `sum`, `(`) | `amount` | `)` | Read `amount` as the inner column of `sum(amount)`. |

Therefore:
1. **`sql_parser_create(query)`** calls `sql_lexer_next_token()` **twice** to fill both slots of the window (`current_token` and `peek_token`).
2. **`parser_advance(parser)`** slides the window right by 1 slot: `current_token = peek_token`, and pulls one new token from the lexer into `peek_token`.

#### Dependency Chain & Source Code Ordering
In C, the order in which functions are defined in `src/sql_parser.c` directly follows this data flow:
1. `sql_lexer_create()` has zero dependencies on other functions; it only initializes fields (`pos = 0`, `len = strlen(query)`). It should naturally be placed first at the top of the file.
2. `sql_lexer_next_token()` relies on the struct initialized by `sql_lexer_create()`. It implements the character-by-character tokenization loop.
3. `sql_parser_create()` depends directly on `sql_lexer_next_token()` because it calls it to prime the `current_token` and `peek_token` fields.
4. `sql_parser_parse()` and its recursive descent helpers (`parse_select`, `parse_insert`) come last, consuming the primed stream.

### Anatomy of Milestone 2: The Recursive Descent Parser

Once the lexer emits tokens and `sql_parser_create()` primes the 2-token window (`current_token` and `peek_token`), the recursive descent parser walks the grammar rules top-down:

```
                      sql_parser_parse()
                       /              \
        (TOKEN_SELECT)                  (TOKEN_INSERT)
              |                               |
        parse_select()                  parse_insert()
        ├── 1. Projections / Aggs       ├── 1. INTO <table>
        ├── 2. FROM <table>             └── 2. Branch on current_token:
        ├── 3. Optional WHERE                  ├── TOKEN_FROM -> Bulk JSON file
        └── 4. Optional GROUP BY               └── TOKEN_LPAREN -> (cols) VALUES (vals)
```

#### 1. Priming the Window (`sql_parser_create`) & Sliding It (`parser_advance`)
First, `sql_parser_create` initializes the lexer and fetches the first two tokens before constructing the `sql_parser_t` struct:
```c
sql_parser_t sql_parser_create(const char* query) {
    sql_lexer_t lexer = sql_lexer_create(query);
    sql_token_t current = sql_lexer_next_token(&lexer);
    sql_token_t peek = sql_lexer_next_token(&lexer);
    return (sql_parser_t){.lexer = lexer, .current_token = current, .peek_token = peek};
}
```
* **C Sequence Point Note:** Why compute `current` and `peek` on separate lines *before* `(sql_parser_t){.lexer = lexer, ...}`? Because each `sql_lexer_next_token(&lexer)` call mutates `lexer.pos`! In C11, the evaluation order of fields inside a compound literal `{.lexer = lexer, .current_token = sql_lexer_next_token(&lexer), ...}` is **unsequenced**—if `.lexer = lexer` were copied *before* the two `next_token(&lexer)` calls ran, `parser.lexer.pos` would still be `0`! Computing `current` and `peek` on separate statements first guarantees `lexer.pos` has already advanced past the first two tokens when `.lexer = lexer` copies it.

Every time the parser consumes `current_token`, it slides the 1-token lookahead window forward:
```c
static void parser_advance(sql_parser_t* parser) {
    parser->current_token = parser->peek_token;
    parser->peek_token = sql_lexer_next_token(&parser->lexer);
}
```

#### 2. Top-Level Dispatcher (`sql_parser_parse`)
In Backus-Naur Form (BNF), the root grammar production of our SQL dialect is:
```bnf
<query> ::= <select_stmt> | <insert_stmt>
```
In Recursive Descent, each grammar rule becomes a C function. Therefore, `sql_parser_parse()` is the **root function** representing `<query>`:
1. **Sanitize Output:** It zeroes out `*out_query = (ast_query_t){0};` so no uninitialized stack garbage remains in unused AST fields.
2. **Inspect the First Token (`current_token`):** Because `sql_parser_create()` already primed `current_token` with the very first token of the query string, `sql_parser_parse()` simply checks what keyword the query starts with:
   - Starts with `TOKEN_SELECT` -> hand off the `parser` and `out_query` pointers to `parse_select(parser, out_query)`.
   - Starts with `TOKEN_INSERT` -> hand off the `parser` and `out_query` pointers to `parse_insert(parser, out_query)`.
   - Starts with anything else (e.g., `FROM` or a number) -> invalid SQL statement, immediately return `false`.

```c
bool sql_parser_parse(sql_parser_t* parser, ast_query_t* out_query) {
    if (!parser || !out_query) return false;
    *out_query = (ast_query_t){0};

    if (parser->current_token.type == TOKEN_SELECT) {
        return sql_parser_parse_select(parser, out_query);
    } else if (parser->current_token.type == TOKEN_INSERT) {
        return sql_parser_parse_insert(parser, out_query);
    }
    return false;
}
```
* **C Pointer Quirk: Why `return false;` instead of `out_query = NULL;`?**
  In C, pointer arguments are passed **by value** (the function receives a local copy of the memory address `&ast`). Writing `out_query = NULL;` inside `sql_parser_parse()` only sets the local variable `out_query` to `NULL` right before the function exits—the caller's `ast` variable in `main()` is completely unaffected! Instead, C functions with out-parameters (`ast_query_t* out_query`) communicate success or failure via their **return value** (`bool`: `true` if parsing succeeded, `false` if a syntax error occurred).

#### 3. The 4-Clause Strategy for `sql_parser_parse_select`
*(Note: include `<stdlib.h>` at the top of `src/sql_parser.c` for `atoi()`).*

Every SQL `SELECT` query in our dialect follows a strict left-to-right sequence of **4 clauses** (2 required, 2 optional):

```
[Clause 1: SELECT List] ---> [Clause 2: FROM Table] ---> [Clause 3: WHERE? (opt)] ---> [Clause 4: GROUP BY? (opt)]
 (Loops until TOKEN_FROM)     (Expects IDENTIFIER)        (If TOKEN_WHERE)              (If TOKEN_GROUP)
```

Because SQL clauses always appear in this exact order, `sql_parser_parse_select` is just 4 sequential blocks of code:

1. **Clause 1 — Select List (`SELECT <expr>, <expr> ...` — Required):**
   - Set `out_query->stmt_type = STMT_SELECT;` and `parser_advance(parser);` past `TOKEN_SELECT`.
   - Loop while `current_token.type != TOKEN_FROM && current_token.type != TOKEN_EOF`:
     - Verify `current_token.type == TOKEN_IDENTIFIER`.
     - **Use the 1-token lookahead (`peek_token`)** to distinguish `sum(amount)` from `user_id`:
       ```c
       ast_expr_t *expr = &out_query->select_exprs[out_query->num_select_exprs++];
       if (parser->peek_token.type == TOKEN_LPAREN) {
           if (strcasecmp(parser->current_token.text, "sum") == 0) expr->type = AST_AGG_SUM;
           else if (strcasecmp(parser->current_token.text, "max") == 0) expr->type = AST_AGG_MAX;
           else if (strcasecmp(parser->current_token.text, "min") == 0) expr->type = AST_AGG_MIN;
           else if (strcasecmp(parser->current_token.text, "count") == 0) expr->type = AST_AGG_COUNT;
           else return false;

           parser_advance(parser); // consume function name
           parser_advance(parser); // consume '('
           if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
           strcpy(expr->column_name, parser->current_token.text);
           parser_advance(parser); // consume column name
           if (parser->current_token.type != TOKEN_RPAREN) return false;
           parser_advance(parser); // consume ')'
       } else {
           expr->type = AST_PROJECTION;
           strcpy(expr->column_name, parser->current_token.text);
           parser_advance(parser); // consume column name
       }
       if (parser->current_token.type == TOKEN_COMMA) {
           parser_advance(parser); // skip ','
       }
       ```
     * **Common C Parser Pitfalls in Clause 1:**
       1. **Consume `TOKEN_SELECT` First:** When `sql_parser_parse_select` starts, `current_token` is still `TOKEN_SELECT`! Always call `parser_advance(parser);` before entering the `while` loop, or `current_token.type == TOKEN_IDENTIFIER` will immediately be false.
       2. **`strcasecmp(a, b) == 0` Trap:** In C, `strcmp` and `strcasecmp` return **`0` when strings are equal** (representing a difference of `0`). Writing `if (strcasecmp(tok, "SUM"))` without `== 0` evaluates to true when the token is **NOT** `"SUM"`!
       3. **Consume Both `(` and `)` Around Aggregate Columns:** In `sum(amount)`, the token sequence is `[sum] -> [(] -> [amount] -> [)]`. After advancing past `sum`, you must also advance past `(` (`TOKEN_LPAREN`) before reading `amount`, and then advance past `amount` and `)` (`TOKEN_RPAREN`).
       4. **Advance Past Plain Columns & Commas:** In the `AST_PROJECTION` branch, remember to call `parser_advance(parser)` after copying `column_name`, and if the next token is `TOKEN_COMMA` (`,`), call `parser_advance(parser)` again so the next iteration of the `while` loop lands on the next identifier!
       5. **Optional `AS <alias>` Requires Two Advances:** If you support `AS alias_name` (`strcasecmp(parser->current_token.text, "AS") == 0`), remember to call `parser_advance(parser)` once to step from `"AS"` onto `alias_name`, copy `alias_name` into `expr.alias`, and then call `parser_advance(parser)` a **second time** to step past `alias_name` onto `,` or `FROM`!
2. **Clause 2 — `FROM` Clause (`FROM <table>` — Required):**
   - When the `while` loop above finishes, `current_token` must be `TOKEN_FROM`.
   - Consume `FROM`, then read the table name (`TOKEN_IDENTIFIER`) into `out_query->table_name`:
     ```c
     if (parser->current_token.type != TOKEN_FROM) return false;
     parser_advance(parser); // consume FROM
     if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
     strcpy(out_query->table_name, parser->current_token.text);
     parser_advance(parser); // consume table name
     ```
3. **Clause 3 — `WHERE` Clause (`WHERE <col> <op> <val>` — Optional):**
   - Check `if (parser->current_token.type == TOKEN_WHERE)`. If it's not `WHERE`, skip straight to Clause 4!
   - If it is `WHERE`, consume `WHERE` and read the 3 parts of a predicate (`<col>`, `<op>`, `<val>`):
     ```c
     if (parser->current_token.type == TOKEN_WHERE) {
         parser_advance(parser); // consume WHERE
         if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
         ast_predicate_t *pred = &out_query->predicates[out_query->num_predicates++];
         strcpy(pred->column_name, parser->current_token.text);
         parser_advance(parser); // consume column name

         if (parser->current_token.type == TOKEN_EQUALS) pred->op = OP_EQUALS;
         else if (parser->current_token.type == TOKEN_GREATER) pred->op = OP_GREATER_THAN;
         else if (parser->current_token.type == TOKEN_LESS) pred->op = OP_LESS_THAN;
         else return false;
         parser_advance(parser); // consume operator

         if (parser->current_token.type == TOKEN_NUMBER) {
             pred->is_string = false;
             pred->int_val = atoi(parser->current_token.text);
         } else if (parser->current_token.type == TOKEN_STRING) {
             pred->is_string = true;
             strcpy(pred->string_val, parser->current_token.text);
         } else return false;
         parser_advance(parser); // consume value
     }
     ```
4. **Clause 4 — `GROUP BY` Clause (`GROUP BY <col>` — Optional):**
   - Check `if (parser->current_token.type == TOKEN_GROUP)`.
   - Consume `GROUP`, verify and consume `BY`, and read the grouping column into `out_query->group_by_col`:
     ```c
     if (parser->current_token.type == TOKEN_GROUP) {
         parser_advance(parser); // consume GROUP
         if (parser->current_token.type != TOKEN_BY) return false;
         parser_advance(parser); // consume BY
         if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
         strcpy(out_query->group_by_col, parser->current_token.text);
         out_query->has_group_by = true;
         parser_advance(parser); // consume column name
     }
     ```
5. **Clause 5 — `ORDER BY` Clause (`ORDER BY <col> [ASC|DESC]` — Optional):**
   - Connects directly to our Day 4 `grade_up`/`grade_down` and Day 15 External Merge Sort!
   - Also add `ORDER` -> `TOKEN_ORDER`, `ASC` -> `TOKEN_ASC`, and `DESC` -> `TOKEN_DESC` to the lexer's keyword checks.
   - Check `if (parser->current_token.type == TOKEN_ORDER)`:
     ```c
     if (parser->current_token.type == TOKEN_ORDER) {
         parser_advance(parser); // consume ORDER
         if (parser->current_token.type != TOKEN_BY) return false;
         parser_advance(parser); // consume BY
         if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
         strcpy(out_query->order_by_col, parser->current_token.text);
         out_query->has_order_by = true;
         out_query->order_desc = false; // default ASC
         parser_advance(parser); // consume column name

         if (parser->current_token.type == TOKEN_DESC) {
             out_query->order_desc = true;
             parser_advance(parser);
         } else if (parser->current_token.type == TOKEN_ASC) {
             parser_advance(parser);
         }
     }
     ```

#### 4. How `sql_parser_parse_insert` Consumes Tokens
1. **Header (`INSERT INTO <table>`):**
   - Set `out_query->stmt_type = STMT_INSERT`, consume `TOKEN_INSERT`, verify and consume `TOKEN_INTO`, and read the table name (`TOKEN_IDENTIFIER`):
     ```c
     out_query->stmt_type = STMT_INSERT;
     parser_advance(parser); // consume INSERT
     if (parser->current_token.type != TOKEN_INTO) return false;
     parser_advance(parser); // consume INTO
     if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
     memcpy(out_query->table_name, parser->current_token.text, parser->current_token.length);
     parser_advance(parser); // consume table name
     ```
2. **Branch A — Bulk JSON (`FROM 'file.json'`):**
   - If `current_token.type == TOKEN_FROM`, consume `FROM`, read `TOKEN_STRING` into `out_query->from_file_path`, set `out_query->is_bulk_insert = true`, and return `true`:
     ```c
     if (parser->current_token.type == TOKEN_FROM) {
         parser_advance(parser); // consume FROM
         if (parser->current_token.type != TOKEN_STRING) return false;
         out_query->is_bulk_insert = true;
         memcpy(out_query->from_file_path, parser->current_token.text, parser->current_token.length);
         parser_advance(parser); // consume file path
         return true;
     }
     ```
3. **Branch B — Row Values (`(col1, col2) VALUES (val1, val2)`):**
   - Verify and advance past `TOKEN_LPAREN`, loop over comma-separated column names until `TOKEN_RPAREN`, consume `TOKEN_VALUES`, and loop over comma-separated values inside `(...)`:
     ```c
     if (parser->current_token.type != TOKEN_LPAREN) return false;
     parser_advance(parser); // consume '('

     while (parser->current_token.type == TOKEN_IDENTIFIER) {
         memcpy(out_query->insert_cols[out_query->num_insert_cols++],
                parser->current_token.text, parser->current_token.length);
         parser_advance(parser);
         if (parser->current_token.type == TOKEN_COMMA) parser_advance(parser);
     }
     if (parser->current_token.type != TOKEN_RPAREN) return false;
     parser_advance(parser); // consume ')'

     if (parser->current_token.type != TOKEN_VALUES) return false;
     parser_advance(parser); // consume VALUES
     if (parser->current_token.type != TOKEN_LPAREN) return false;
     parser_advance(parser); // consume '('

     while (parser->current_token.type == TOKEN_NUMBER ||
            parser->current_token.type == TOKEN_STRING) {
         ast_value_t *val = &out_query->insert_vals[out_query->num_insert_vals++];
         if (parser->current_token.type == TOKEN_NUMBER) {
             val->is_string = false;
             val->int_val = atoi(parser->current_token.text);
         } else {
             val->is_string = true;
             memcpy(val->string_val, parser->current_token.text, parser->current_token.length);
         }
         parser_advance(parser);
         if (parser->current_token.type == TOKEN_COMMA) parser_advance(parser);
     }
     if (parser->current_token.type != TOKEN_RPAREN) return false;
     parser_advance(parser); // consume ')'
     return out_query->num_insert_cols == out_query->num_insert_vals;
     ```

Putting all three parts together gives the complete `sql_parser_parse_insert` helper:
```c
static bool sql_parser_parse_insert(sql_parser_t *parser, ast_query_t *out_query) {
    out_query->stmt_type = STMT_INSERT;
    parser_advance(parser); // consume INSERT

    if (parser->current_token.type != TOKEN_INTO) return false;
    parser_advance(parser); // consume INTO

    if (parser->current_token.type != TOKEN_IDENTIFIER) return false;
    memcpy(out_query->table_name, parser->current_token.text, parser->current_token.length);
    parser_advance(parser); // consume table name

    // Branch A: Bulk JSON (INSERT INTO <table> FROM '<path>')
    if (parser->current_token.type == TOKEN_FROM) {
        parser_advance(parser); // consume FROM
        if (parser->current_token.type != TOKEN_STRING) return false;
        out_query->is_bulk_insert = true;
        memcpy(out_query->from_file_path, parser->current_token.text, parser->current_token.length);
        parser_advance(parser); // consume file path
        return true;
    }

    // Branch B: Row Values (INSERT INTO <table> (col1, col2) VALUES (val1, val2))
    if (parser->current_token.type != TOKEN_LPAREN) return false;
    parser_advance(parser); // consume '('

    while (parser->current_token.type == TOKEN_IDENTIFIER) {
        memcpy(out_query->insert_cols[out_query->num_insert_cols++],
               parser->current_token.text, parser->current_token.length);
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_COMMA) parser_advance(parser);
    }
    if (parser->current_token.type != TOKEN_RPAREN) return false;
    parser_advance(parser); // consume ')'

    if (parser->current_token.type != TOKEN_VALUES) return false;
    parser_advance(parser); // consume VALUES
    if (parser->current_token.type != TOKEN_LPAREN) return false;
    parser_advance(parser); // consume '('

    while (parser->current_token.type == TOKEN_NUMBER ||
           parser->current_token.type == TOKEN_STRING) {
        ast_value_t *val = &out_query->insert_vals[out_query->num_insert_vals++];
        if (parser->current_token.type == TOKEN_NUMBER) {
            val->is_string = false;
            val->int_val = atoi(parser->current_token.text);
        } else {
            val->is_string = true;
            memcpy(val->string_val, parser->current_token.text, parser->current_token.length);
        }
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_COMMA) parser_advance(parser);
    }
    if (parser->current_token.type != TOKEN_RPAREN) return false;
    parser_advance(parser); // consume ')'

    return out_query->num_insert_cols == out_query->num_insert_vals;
}
```

## 3. The Final Listing: `src/sql_parser.c`

Here is the complete, self-contained implementation of the SQL Lexer, recursive descent parser, and AST generator:

```c
#include "simpledb/sql_parser.h"
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

sql_lexer_t sql_lexer_create(const char *query) {
    if (!query) {
        return (sql_lexer_t){.input = "", .len = 0, .pos = 0};
    }
    size_t len = strlen(query);
    sql_lexer_t lexer = (sql_lexer_t){.input = query, .len = len, .pos = 0};
    return lexer;
}

sql_token_t sql_lexer_next_token(sql_lexer_t *lexer) {

    while (lexer->pos < lexer->len && isspace((unsigned char)lexer->input[lexer->pos])) {
        lexer->pos++;
    }

    if (lexer->pos >= lexer->len) {
        return (sql_token_t){.type = TOKEN_EOF, .text = "", .length = 0};
    }
    char c = lexer->input[lexer->pos];

    switch (c) {
    case ',':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_COMMA, .text = ",", .length = 1};
    case '(':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_LPAREN, .text = "(", .length = 1};
    case ')':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_RPAREN, .text = ")", .length = 1};
    case '=':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_EQUALS, .text = "=", .length = 1};
    case '>':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_GREATER, .text = ">", .length = 1};
    case '<':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_LESS, .text = "<", .length = 1};
    case ';':
        lexer->pos++;
        return (sql_token_t){.type = TOKEN_EOF, .text = ";", .length = 1};
    }

    // quoted text
    if (c == '\'') {
        lexer->pos++; // skip opening quote
        size_t start = lexer->pos;
        while (lexer->pos < lexer->len && lexer->input[lexer->pos] != '\'') {
            lexer->pos++;
        }
        size_t len = lexer->pos - start;
        sql_token_t tok = {.type = TOKEN_STRING, .length = (int)len};
        if (len >= sizeof(tok.text)) {
            return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
        }
        memcpy(tok.text, &lexer->input[start], len);
        tok.text[len] = '\0';
        if (lexer->pos < lexer->len && lexer->input[lexer->pos] == '\'') {
            lexer->pos++; // skip closing quote
        }
        return tok;
    }

    // numbers
    if (isdigit((unsigned char)c)) {
        size_t start = lexer->pos;
        do {
            lexer->pos++;
        } while (lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos]));

        // float
        if (lexer->pos + 1 < lexer->len && lexer->input[lexer->pos] == '.' &&
            isdigit((unsigned char)lexer->input[lexer->pos + 1])) {
            do {
                lexer->pos++;
            } while (lexer->pos < lexer->len && isdigit((unsigned char)lexer->input[lexer->pos]));
        }

        size_t len = lexer->pos - start;
        sql_token_t tok = {.type = TOKEN_NUMBER, .length = (int)len};

        if (len >= sizeof(tok.text)) {
            return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
        }
        memcpy(tok.text, &lexer->input[start], len);
        tok.text[len] = '\0';
        return tok;
    }

    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = lexer->pos;
        do {
            lexer->pos++;
        } while (lexer->pos < lexer->len &&
                 (isalnum((unsigned char)lexer->input[lexer->pos]) ||
                  lexer->input[lexer->pos] == '_' || lexer->input[lexer->pos] == '.'));
        size_t len = lexer->pos - start;
        sql_token_t tok = {.type = TOKEN_IDENTIFIER, .length = (int)len};
        if (len >= sizeof(tok.text)) {
            return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
        }
        memcpy(tok.text, &lexer->input[start], len);
        tok.text[len] = '\0';
        if (strcasecmp(tok.text, "SELECT") == 0)
            tok.type = TOKEN_SELECT;
        else if (strcasecmp(tok.text, "FROM") == 0)
            tok.type = TOKEN_FROM;
        else if (strcasecmp(tok.text, "WHERE") == 0)
            tok.type = TOKEN_WHERE;
        else if (strcasecmp(tok.text, "GROUP") == 0)
            tok.type = TOKEN_GROUP;
        else if (strcasecmp(tok.text, "BY") == 0)
            tok.type = TOKEN_BY;
        else if (strcasecmp(tok.text, "INSERT") == 0)
            tok.type = TOKEN_INSERT;
        else if (strcasecmp(tok.text, "INTO") == 0)
            tok.type = TOKEN_INTO;
        else if (strcasecmp(tok.text, "VALUES") == 0)
            tok.type = TOKEN_VALUES;
        else if (strcasecmp(tok.text, "ORDER") == 0)
            tok.type = TOKEN_ORDER;
        else if (strcasecmp(tok.text, "ASC") == 0)
            tok.type = TOKEN_ASC;
        else if (strcasecmp(tok.text, "DESC") == 0)
            tok.type = TOKEN_DESC;
        return tok;
    }

    lexer->pos++;
    return (sql_token_t){.type = TOKEN_ERROR, .length = 0};
}

sql_parser_t sql_parser_create(const char *query) {
    sql_lexer_t lexer = sql_lexer_create(query);
    sql_token_t current = sql_lexer_next_token(&lexer);
    sql_token_t peek = sql_lexer_next_token(&lexer);
    return (sql_parser_t){.lexer = lexer, .current_token = current, .peek_token = peek};
}

static void parser_advance(sql_parser_t *parser) {
    parser->current_token = parser->peek_token;
    parser->peek_token = sql_lexer_next_token(&parser->lexer);
}

static bool sql_parser_parse_insert(sql_parser_t *parser, ast_query_t *out_query) {
    out_query->stmt_type = STMT_INSERT;
    parser_advance(parser); // consume INSERT

    if (parser->current_token.type != TOKEN_INTO)
        return false;
    parser_advance(parser); // consume INTO

    if (parser->current_token.type != TOKEN_IDENTIFIER)
        return false;
    memcpy(out_query->table_name, parser->current_token.text, parser->current_token.length);
    parser_advance(parser); // consume table name

    // Branch A: Bulk JSON (INSERT INTO <table> FROM '<path>')
    if (parser->current_token.type == TOKEN_FROM) {
        parser_advance(parser); // consume FROM
        if (parser->current_token.type != TOKEN_STRING)
            return false;
        out_query->is_bulk_insert = true;
        memcpy(out_query->from_file_path, parser->current_token.text, parser->current_token.length);
        parser_advance(parser); // consume file path
        return true;
    }

    // Branch B: Row Values (INSERT INTO <table> (col1, col2) VALUES (val1, val2))
    if (parser->current_token.type != TOKEN_LPAREN)
        return false;
    parser_advance(parser); // consume '('

    while (parser->current_token.type == TOKEN_IDENTIFIER) {
        memcpy(out_query->insert_cols[out_query->num_insert_cols++], parser->current_token.text,
               parser->current_token.length);
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_COMMA)
            parser_advance(parser);
    }
    if (parser->current_token.type != TOKEN_RPAREN)
        return false;
    parser_advance(parser); // consume ')'

    if (parser->current_token.type != TOKEN_VALUES)
        return false;
    parser_advance(parser); // consume VALUES
    if (parser->current_token.type != TOKEN_LPAREN)
        return false;
    parser_advance(parser); // consume '('

    while (parser->current_token.type == TOKEN_NUMBER ||
           parser->current_token.type == TOKEN_STRING) {
        ast_value_t *val = &out_query->insert_vals[out_query->num_insert_vals++];
        if (parser->current_token.type == TOKEN_NUMBER) {
            val->is_string = false;
            val->int_val = atoi(parser->current_token.text);
        } else {
            val->is_string = true;
            memcpy(val->string_val, parser->current_token.text, parser->current_token.length);
        }
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_COMMA)
            parser_advance(parser);
    }
    if (parser->current_token.type != TOKEN_RPAREN)
        return false;
    parser_advance(parser); // consume ')'

    return out_query->num_insert_cols == out_query->num_insert_vals;
}

static bool sql_parser_parse_select(sql_parser_t *parser, ast_query_t *out_query) {

    out_query->stmt_type = STMT_SELECT;
    parser_advance(parser);

    // SELET ... expressions
    size_t n = 0;
    while (parser->current_token.type == TOKEN_IDENTIFIER) {
        ast_expr_t expr = (ast_expr_t){0};
        if (parser->peek_token.type == TOKEN_LPAREN) {
            if (strcasecmp(parser->current_token.text, "SUM") == 0)
                expr.type = AST_AGG_SUM;
            else if (strcasecmp(parser->current_token.text, "COUNT") == 0)
                expr.type = AST_AGG_COUNT;
            else if (strcasecmp(parser->current_token.text, "MAX") == 0)
                expr.type = AST_AGG_MAX;
            else if (strcasecmp(parser->current_token.text, "MIN") == 0)
                expr.type = AST_AGG_MIN;
            else
                return false;
            parser_advance(parser); // consume function name (lands on '(')
            parser_advance(parser); // consume '(' (lands on column name)
            if (parser->current_token.type != TOKEN_IDENTIFIER)
                return false;
            memcpy(expr.column_name, parser->current_token.text, parser->current_token.length);
            parser_advance(parser); // consume column name (lands on ')')
            if (parser->current_token.type != TOKEN_RPAREN)
                return false;
            parser_advance(parser); // consume ')'
        } else {
            expr.type = AST_PROJECTION;
            memcpy(expr.column_name, parser->current_token.text, parser->current_token.length);
            parser_advance(parser);
        }
        if (parser->peek_token.type == TOKEN_IDENTIFIER &&
            strcasecmp(parser->current_token.text, "AS") == 0) {
            parser_advance(parser);
            memcpy(expr.alias, parser->current_token.text, parser->current_token.length);
            parser_advance(parser);
        }
        out_query->select_exprs[n++] = expr;
        if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
        }
    }
    out_query->num_select_exprs = n;

    // FROM 'table'
    if (parser->current_token.type != TOKEN_FROM)
        return false;
    parser_advance(parser); // consume FROM
    if (parser->current_token.type != TOKEN_IDENTIFIER)
        return false;
    memcpy(out_query->table_name, parser->current_token.text, parser->current_token.length);
    parser_advance(parser); // consume table name

    // WHERE ... cond
    if (parser->current_token.type == TOKEN_WHERE) {
        parser_advance(parser); // consume WHERE
        if (parser->current_token.type != TOKEN_IDENTIFIER)
            return false;
        ast_predicate_t *pred = &out_query->predicates[out_query->num_predicates++];
        memcpy(pred->column_name, parser->current_token.text, parser->current_token.length);
        parser_advance(parser); // consume column name
        if (parser->current_token.type == TOKEN_EQUALS)
            pred->op = OP_EQUALS;
        else if (parser->current_token.type == TOKEN_GREATER)
            pred->op = OP_GREATER_THAN;
        else if (parser->current_token.type == TOKEN_LESS)
            pred->op = OP_LESS_THAN;
        else
            return false;
        parser_advance(parser); // consume operator
        if (parser->current_token.type == TOKEN_NUMBER) {
            pred->is_string = false;
            pred->int_val = atoi(parser->current_token.text);
        } else if (parser->current_token.type == TOKEN_STRING) {
            pred->is_string = true;
            memcpy(pred->string_val, parser->current_token.text, parser->current_token.length);
        } else {
            return false;
        }
        parser_advance(parser); // consume value
    }

    // GROUP BY
    if (parser->current_token.type == TOKEN_GROUP) {
        parser_advance(parser); // consume GROUP
        if (parser->current_token.type != TOKEN_BY)
            return false;
        parser_advance(parser); // consume BY
        if (parser->current_token.type != TOKEN_IDENTIFIER)
            return false;
        memcpy(out_query->group_by_col, parser->current_token.text, parser->current_token.length);
        out_query->has_group_by = true;
        parser_advance(parser); // consume column name
    }

    // order by ...
    if (parser->current_token.type == TOKEN_ORDER) {
        parser_advance(parser); // consume ORDER
        if (parser->current_token.type != TOKEN_BY)
            return false;
        parser_advance(parser); // consume BY
        if (parser->current_token.type != TOKEN_IDENTIFIER)
            return false;
        memcpy(out_query->order_by_col, parser->current_token.text, parser->current_token.length);
        out_query->has_order_by = true;
        out_query->order_desc = false; // default ASC
        parser_advance(parser);        // consume column name
        if (parser->current_token.type == TOKEN_DESC) {
            out_query->order_desc = true;
            parser_advance(parser);
        } else if (parser->current_token.type == TOKEN_ASC) {
            parser_advance(parser);
        }
    }
    return true;
}

bool sql_parser_parse(sql_parser_t *parser, ast_query_t *out_query) {
    if (!parser || !out_query)
        return false;
    *out_query = (ast_query_t){0};

    if (parser->current_token.type == TOKEN_INSERT) {
        return sql_parser_parse_insert(parser, out_query);
    } else if (parser->current_token.type == TOKEN_SELECT) {
        return sql_parser_parse_select(parser, out_query);
    } else {
        return false;
    }
}
```

With the parser verified and complete, we now have an expressive front-end that translates raw SQL strings into a structured `ast_query_t`. In **Day 21: The Interactive REPL CLI & Query Engine**, we take this AST and wire it into an interactive command-line interface, streaming NDJSON ingestion via `yyjson`, dynamic schema inference, and vectorized execution pipeline coordination!

---

> **Day 20 Branch**: View the exact code for this day at [dmanchon/simpledb (`day20`)](https://github.com/dmanchon/simpledb/tree/day20) ([diff from `day19`](https://github.com/dmanchon/simpledb/compare/day19...day20)).

