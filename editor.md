# Fast Vim / Neovim Editing Guide for Zed Editor (C11 Edition)

This guide is designed to transform your editing speed in **Zed** using modern Neovim/Vim modal editing techniques, text objects, search/replace patterns, registers, macros, LSP shortcuts, code navigation, function documentation / man pages, autoformatting, debugging, advanced Zed settings, and community links.

---

## 1. Viewing Function Documentation & Man Pages in Zed

When writing C11 system code (e.g. `posix_memalign`, `mmap`, `printf`), you can inspect function signatures, documentation, and system man pages directly inside Zed:

### A. 1-Keystroke Hover Documentation (LSP)
- **Vim Shortcut**: **`gh`** (Hover Docs — press `g` then `h` over any function).
- **Command Palette**: `Cmd + Shift + P` $\rightarrow$ `editor: show hover`.
- **Mouse**: Hover your mouse cursor over any C function name.
- *Shows*: Function signature, parameters, return type, and C standard documentation in an instant popover tooltip.

### B. 1-Keystroke Automatic Man Page in Terminal Console (`Space m` / `ctrl-m`)
Stream the complete C `man` page directly into Zed's integrated terminal console dock!

#### Step 1: Configured in `.zed/tasks.json`
```json
[
  {
    "label": "Open Man Page",
    "command": "WORD=\"${ZED_SELECTED_TEXT:-$ZED_SYMBOL}\"; WORD=\"${WORD:-posix_memalign}\"; man 3 \"$WORD\" || man 2 \"$WORD\" || man \"$WORD\"",
    "cwd": "$ZED_WORKTREE_ROOT",
    "reveal": "always",
    "reveal_target": "dock",
    "use_new_terminal": true,
    "allow_concurrent_runs": true
  }
]
```

#### Step 2: Bind in `~/.config/zed/keymap.json`
```json
[
  {
    "context": "VimControl && !menu",
    "bindings": {
      "ctrl-m": ["task::Spawn", { "task_name": "Open Man Page" }],
      "space m": ["task::Spawn", { "task_name": "Open Man Page" }]
    }
  }
]
```

Now, pressing **`ctrl-m`** (or **`space m`**) opens an interactive terminal session running `man`:
- **Scroll**: Use trackpad/mouse wheel, `j`/`k`, or `Ctrl+d`/`Ctrl+u`.
- **Search**: Press **`/`**, type search query, and press `Enter`. Next match: `n`, Previous: `N`.
- **Quit**: Press **`q`**.

### C. Go to System Header Definition
- **Vim Shortcut**: **`gd`** (Go to Definition).
- Jump directly into system header files (e.g. `<stdlib.h>` or `<unistd.h>`) to read declaration comments.

---

## 2. Generating `compile_commands.json` Symlink for LSP (`clangd`)

LSP language servers like **`clangd`** require a `compile_commands.json` file in the root of your project directory to understand include paths (`-Iinclude`), compiler flags (`-std=c11`), and macro defines.

---

## 3. Building & Running Tests in Zed Tasks (`.zed/tasks.json`)

Zed supports running custom build scripts, test suites, and executables directly inside the editor using project-level task configurations.

> [!NOTE]
> **Keybindings vs Tasks in Zed**: Zed manages `keymap.json` globally (`~/.config/zed/keymap.json`), whereas `.zed/tasks.json` is project-specific. By mapping global shortcuts to generic task names like `"Build Project"` and `"Run Tests"`, the same shortcuts will execute whatever build/test logic is configured in the current project's `.zed/tasks.json`.

### A. Workspace Tasks Configuration (`.zed/tasks.json`)
This project includes pre-configured tasks in `.zed/tasks.json`:

```json
[
  {
    "label": "Build Project",
    "command": "cmake -B build && cmake --build build",
    "cwd": "$ZED_WORKTREE_ROOT",
    "use_new_terminal": false,
    "allow_concurrent_runs": false
  },
  {
    "label": "Run Tests",
    "command": "ctest --test-dir build --output-on-failure",
    "cwd": "$ZED_WORKTREE_ROOT",
    "use_new_terminal": false,
    "allow_concurrent_runs": false
  },
  {
    "label": "Build & Run Tests",
    "command": "cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure",
    "cwd": "$ZED_WORKTREE_ROOT",
    "use_new_terminal": false,
    "allow_concurrent_runs": false
  },
  {
    "label": "Run CLI Executable",
    "command": "./build/simpledb",
    "cwd": "$ZED_WORKTREE_ROOT",
    "use_new_terminal": false,
    "allow_concurrent_runs": false
  }
]
```

### B. Triggering Tasks in Zed
1. **Spawn Task Selector**:
   - Shortcut: `Option + Shift + T` or `Cmd + Shift + P` $\rightarrow$ type `task: spawn`.
   - Select **Build Project**, **Run Tests**, or **Build & Run Tests**.
2. **Rerun Last Task**:
   - `Cmd + Shift + P` $\rightarrow$ type `task: rerun`.

### C. Global Keybindings Linked to Project Tasks (`~/.config/zed/keymap.json`)
Open your global keymap (`Cmd + Shift + P` $\rightarrow$ `zed: open keymap`).

To use **`space`** as a leader key without Vim's default movement taking over, you **MUST** specify this exact context:

```json
[
  {
    "context": "VimControl && !menu",
    "bindings": {
      "space space": "task::Spawn",
      "space b": ["task::Spawn", { "task_name": "Build Project" }],
      "space t": ["task::Spawn", { "task_name": "Run Tests" }],
      "space m": ["task::Spawn", { "task_name": "Open Man Page" }],
      "ctrl-b": ["task::Spawn", { "task_name": "Build Project" }],
      "ctrl-t": ["task::Spawn", { "task_name": "Run Tests" }],
      "ctrl-m": ["task::Spawn", { "task_name": "Open Man Page" }]
    }
  }
]
```

---

## 4. Enabling Vim Mode & Neovim Features in Zed

In Zed, open Settings (`Cmd+,` or `Cmd+Shift+P` -> `zed: open settings`) and configure your Vim environment:

```json
{
  "vim_mode": true,
  "format_on_save": "on",
  "vim": {
    "toggle_relative_line_numbers": true,
    "use_system_clipboard": "always"
  },
  "inlay_hints": {
    "enabled": true
  }
}
```

---

## 5. Core Mental Model: Grammatical Editing

Vim commands follow a **Verb + Count + Text Object (Noun)** structure:

$$\text{Action (Verb)} + [\text{Count}] + \text{Motion / Text Object (Noun)}$$

| Verbs (Action) | Text Objects (Nouns) | Examples |
| :--- | :--- | :--- |
| `d` (delete) | `w` (word) | `dw` -> delete word |
| `c` (change = delete & Insert mode) | `iw` (inner word) | `ciw` -> replace current word |
| `y` (yank / copy) | `i"` (inside quotes) | `yi"` -> copy inside `"..."` |
| `v` (visual select) | `a(` (around parentheses) | `va(` -> select `(...)` including parens |
| `>` / `<` (indent / unindent) | `ip` (inner paragraph) | `>ip` -> indent current block |

---

## 6. Modern Neovim-Compatible Operators & Motion Tricks

### Surround Operators (Neovim `vim-surround` compatible)
- **`ysiw"`**: Surround inner word with double quotes `"..."`.
- **`cs"'`**: Change surrounding double quotes `"..."` to single quotes `'...'`.
- **`ds"`**: Delete surrounding double quotes `"..."`.

### Commenting & Case Conversion
- **`gcc`**: Toggle comment on current line.
- **`gc`**: Toggle comment on visual selection.
- **`guw`** / **`gUw`**: Convert word to lowercase / uppercase.

### Dragging Lines Up / Down
- **`J` / `K`** in Visual mode: Drag selected lines down / up.
- **`Option + ↓ / ↑`**: Standard Zed line moving shortcut.

---

---

## 7. Multi-Cursor & Multiline Occurrence Editing in Zed

There are 4 fast ways to select a word and modify all occurrences across the buffer or project in Zed:

### A. Semantic LSP Rename Symbol (Recommended for Code)
Place your cursor on any variable/function and press:
* **Vim Mode**: **`space r`** or **`ctrl + c r`**
* **Standard Shortcut**: **`F2`** or `Cmd + Shift + P` $\rightarrow$ `editor: rename`
* *What it does*: Uses language server (`clangd`) to safely rename the symbol across the entire buffer and project files without affecting comments or unrelated variables.

### B. Select All Occurrences (Multi-Cursor)
Highlight a word or place your cursor on it, then press:
* **Select ALL Matches in Buffer**: **`Cmd + Shift + L`** (Mac) / **`Ctrl + Shift + L`** (Linux)
* **Select NEXT Match One-by-One**: **`Cmd + D`** (Mac) / **`Ctrl + D`** (Linux)
* *What it does*: Spawns a cursor on **every** matching word simultaneously! Now whatever you type, delete, or replace happens on all lines in real-time.

### C. Vim Search & Substitute Command (`:%s`)
In Normal mode, use standard Vim substitution:
```vim
:%s/old_name/new_name/g
```
To substitute only within a selected range of lines:
1. Select lines in Visual mode (`v` or `V`).
2. Type `:s/old_name/new_name/g` $\rightarrow$ press `Enter`.

### E. Neovim Master Technique: `cgn` + `.` (Dot Repeat Trick)
In pure Vim / Neovim without plugins, the fastest way to replace words one-by-one with full control:

1. Place cursor on the target word.
2. Press **`*`** (searches and highlights the word under cursor).
3. Type **`cgn`** (*Change Next Match*) $\rightarrow$ type your replacement word $\rightarrow$ press **`<Esc>`**.
4. Now press **`.`** (dot command) to repeat the exact replacement on the next match!
   * Press **`.`** to change the next occurrence.
   * Press **`n`** to skip an occurrence without changing it.

### F. Neovim LSP Rename (`<leader>rn` / `grn`)
In modern Neovim (built-in LSP, LazyVim, NvChad):
* Place cursor on symbol $\rightarrow$ press **`<leader>rn`** (or **`grn`** / **`:lua vim.lsp.buf.rename()`**).
* Type new name $\rightarrow$ press **`<Enter>`**.

### G. Neovim Instant Word Substitution (`<C-r><C-w>`)
To auto-fill the word under cursor into Neovim's substitution command:
* Place cursor on word $\rightarrow$ type **`:%s/<C-r><C-w>/new_name/g`**
  *(Pressing `Ctrl + r` then `Ctrl + w` automatically pastes the current word under cursor directly into the command bar!)*

---

## 8. Code Navigation, LSP & Fast Symbol Search

### Fast Symbol & File Search
- `Cmd + T` (Mac) / `Ctrl + T`: **Workspace Symbol Search**.
- `Cmd + Shift + O` or `@`: **Document Symbol Outline**.
- `Cmd + P` / `Ctrl + P`: **Fast File Finder**.

### LSP Navigation Shortcuts
- `gd`: **Go to Definition**.
- `gD`: **Go to Type Definition / Implementation**.
- `gr` or `gl`: **Find All References**.
- `gh`: **Hover Docs** (Display signature and type info).
- `]d` / `[d`: Jump to **Next / Previous Diagnostic** (Error/Warning).

---

## 9. Autoformatting & Code Formatting Shortcuts

- **Format Document**: `Cmd + Shift + I` / `Alt + Shift + F`
- **Format Selection**: Highlight code in Visual mode -> `Cmd + Shift + I`

---

## 10. Debugging Code in Zed (C / LLDB)

| Action | Shortcut | Description |
| :--- | :--- | :--- |
| **Toggle Breakpoint** | `F9` or Click Gutter | Sets or removes breakpoint on current line |
| **Start / Continue Debugging** | `F5` / `Cmd + Shift + D` | Launch debug session or continue |
| **Step Over** | `F10` | Execute current line and pause at next line |
| **Step Into** | `F11` | Step into function call under cursor |
| **Step Out** | `Shift + F11` | Step out of current function to caller |
| **Stop Debugging** | `Shift + F5` | Terminate active debug session |

---

## 11. Muscle Memory Master Cheat Sheet

| Task | Key Sequence |
| :--- | :--- |
| **Select All Occurrences (Multi-Cursor)** | **`Cmd + Shift + L`** |
| **Select Next Occurrence** | **`Cmd + D`** |
| **LSP Rename Symbol (All Buffers)** | **`space r`** / **`F2`** |
| **Build Project** | **`Space b`** (configured Task) or `task: spawn` $\rightarrow$ Build Project |
| **Run Tests** | **`Space t`** (configured Task) or `task: spawn` $\rightarrow$ Run Tests |
| **Show Hover Documentation / Signature** | **`gh`** |
| **Instant Man Page for Symbol** | **`Space m`** (configured Task) |
| **Go to Function Definition** | **`gd`** |
| **Workspace Symbol Search** | `Cmd + T` / `Ctrl + T` |
| **Document Symbol Search** | `Cmd + Shift + O` / `@` |
| **Toggle Comment** | `gcc` / `gc` |
| **Autoformat File** | `Cmd + Shift + I` |
| **Toggle Breakpoint** | `F9` |
| **Start / Continue Debugger** | `F5` |

---

## 12. Links & Community Resources

- **Zed Official**: [https://zed.dev](https://zed.dev)
- **Zed Docs**: [https://zed.dev/docs](https://zed.dev/docs)
- **Zed GitHub**: [https://github.com/zed-industries/zed](https://github.com/zed-industries/zed)
- **Neovim Docs**: [https://neovim.io/doc/](https://neovim.io/doc/)
