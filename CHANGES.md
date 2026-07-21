# Atlas Editor - What Was Built

A terminal text editor written from scratch in C. Single file: `main.c`.

## How to Use

```bash
make                  # build
./atlas               # new file
./atlas main.c        # open a file
```

## Keyboard Shortcuts

| Key        | Action                          |
|------------|---------------------------------|
| Arrow keys | Move cursor                     |
| Home/End   | Jump to start/end of line       |
| Page Up/Dn | Scroll one page up/down         |
| Ctrl+S     | Save file                       |
| Ctrl+Q     | Quit (press twice if unsaved)   |
| Ctrl+F     | Search (arrows = next/prev)     |
| Ctrl+D     | Duplicate current line          |
| Ctrl+Z     | Undo                            |
| Ctrl+Y     | Redo                            |
| Backspace  | Delete character before cursor  |
| Delete     | Delete character at cursor      |
| Enter      | Insert newline / split line     |

## Milestones Completed

### 1. Project Setup
- Makefile with gcc, `-std=c11 -Wall -Wextra -g`
- Builds to `./atlas` binary

### 2. Raw Mode
- `termios` to disable canonical mode, echo, signals
- `atexit()` restores terminal on any exit
- Byte-by-byte input with `VMIN=0, VTIME=1`

### 3. Screen Rendering
- `AppendBuffer` write buffer — single `write()` per frame, no flicker
- `\x1b[?25l/h` to hide/show cursor during redraw
- Tilde (`~`) on empty rows, like vim

### 4. Cursor Movement
- Arrow keys via escape sequence parsing (`\x1b[A/B/C/D`)
- `EditorConfig` global struct for all editor state
- Line wrapping: left at col 0 -> prev line end, right at end -> next line start
- Snap cursor when moving between lines of different length

### 5. File Opening
- `erow` struct: `{size, chars}` per line
- `getline()` reads file line by line
- `editorAppendRow` with realloc grow pattern
- Accepts filename from `argv`

### 6. Scrolling
- `rowoff`/`coloff` viewport offsets in EditorConfig
- `editorScroll()` adjusts offsets when cursor leaves screen
- Horizontal scrolling for long lines
- Cursor bounds check against file content, not screen size

### 7. Status Bar + Message Bar
- Inverted-color status bar: filename, line count, dirty flag, position
- Message bar with 5-second auto-expiry
- `editorSetStatusMessage` with printf-style variadic args
- Filetype shown in status bar (after milestone 11)

### 8. Editing
- `editorInsertChar` / `editorDeleteChar` with cursor management
- `editorInsertNewline` splits lines at cursor position
- Backspace at col 0 joins with previous line
- `editorDuplicateLine` (Ctrl+D) — copies current line below
- Extended keys: Delete, Home, End, Page Up, Page Down
- `editorProcessKeypress` centralizes all key handling
- All row operations use `memmove` for safe overlapping memory

### 9. Save to Disk
- `editorRowsToString` serializes buffer to single string
- `editorSave` (Ctrl+S) with `open/write/close` and error messages
- `editorPrompt` for message-bar text input (reused by search)
- "Save As" prompt when no filename set
- Quit confirmation: must press Ctrl+Q twice with unsaved changes

### 10. Search
- `editorFind` (Ctrl+F) with incremental search
- Results update as you type via `editorFindCallback`
- Arrow keys navigate between matches (wraps around file)
- Escape cancels and restores original cursor position
- Search matches highlighted in blue (after milestone 11)

### 11. Syntax Highlighting
- `editorHighlight` enum: NORMAL, NUMBER, STRING, COMMENT, KEYWORD1, KEYWORD2
- `editorSyntax` struct: filetype definitions with extensions, keywords, comment markers
- C language support in HLDB (keywords, types, single/multi-line comments, strings)
- `editorUpdateSyntax` walks each row assigning colors per character
- Multi-line comment tracking cascades across rows
- ANSI color codes: red=numbers, magenta=strings, cyan=comments, yellow=keywords, green=types
- Auto-detects filetype from filename extension

### 12. Undo/Redo
- Ring buffer (circular array) stacks with 1000-entry cap — auto-evicts oldest
- "Inverse command" pattern: each edit records enough data to reverse itself
- 5 operation types: INSERT_CHAR, DELETE_CHAR, JOIN_LINES, INSERT_NEWLINE, DUPLICATE_LINE
- Undo (Ctrl+Z) pops from undo stack, reverses operation, pushes redo entry
- Redo (Ctrl+Y) pops from redo stack, re-applies operation, pushes undo entry
- New edit clears redo stack (standard behavior)
- Cursor position restored on every undo/redo

## Architecture

Everything lives in `main.c` (~1100 lines). Key data structures:

- `erow` — one row: `{size, chars, hl[], hl_open_comment}`
- `AppendBuffer` — write buffer: `{data, len}` for batch screen writes
- `EditorConfig E` — global state: cursor, scroll, rows, filename, dirty, syntax
- `editorSyntax` / `HLDB[]` — filetype definitions for syntax highlighting

## C Concepts Used

- `termios` for raw terminal control
- `ioctl` with `TIOCGWINSZ` for terminal dimensions
- `realloc/malloc/free` for dynamic memory management
- `memmove` for safe overlapping memory copies
- ANSI escape sequences for cursor positioning and colors
- Variadic functions (`va_list`) for printf-style messaging
- `atexit()` for cleanup on any exit path
- Bitwise operations for flag manipulation
- Pointer arithmetic for buffer and array indexing
- `strstr` for substring search
