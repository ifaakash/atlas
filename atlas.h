/* Atlas - terminal text editor */

#ifndef ATLAS_H
#define ATLAS_H

#include <stdio.h>
#include <stdlib.h>    /* atexit() */
#include <unistd.h>    /* read(), STDIN_FILENO */
#include <termios.h>   /* struct termios, tcgetattr, tcsetattr */
#include <string.h>    /* memcpy, strdup */
#include <sys/ioctl.h> /* ioctl, TIOCGWINSZ, struct winsize */
#include <time.h>      /* time, time_t */
#include <stdarg.h>    /* va_list, va_start, va_end for variadic functions */
#include <fcntl.h>     /* open, O_RDWR, O_CREAT, O_TRUNC */
#include <errno.h>     /* errno, for error messages on save failure */
#include <ctype.h>     /* iscntrl — check if a character is a control character */

/* Ctrl key strips bits 5-7, leaving only bits 0-4. So Ctrl+Q = 'q' & 0x1f = 17 */
#define CTRL_KEY(k) ((k) & 0x1f)

/* Flags for which syntax features a filetype supports */
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

#define UNDO_MAX 1000

/* --- Syntax highlighting types --- */

/* Each character in a row gets one of these highlight types */
enum editorHighlight {
	HL_NORMAL = 0,
	HL_NUMBER,       /* numeric literals */
	HL_MATCH,        /* search match highlight */
	HL_STRING,       /* string literals ("..." or '...') */
	HL_COMMENT,      /* single-line comments (// ...) */
	HL_MLCOMMENT,    /* multi-line comments */
	HL_KEYWORD1,     /* language keywords (if, while, return, etc.) */
	HL_KEYWORD2      /* type keywords (int, char, void, etc.) */
};

/*
 * Filetype definition — describes how to highlight a language.
 * Each entry in the HLDB (highlight database) is one of these.
 */
struct editorSyntax {
	char *filetype;                  /* display name, e.g. "c" */
	char **filematch;                /* file extensions, e.g. {".c", ".h", NULL} */
	char **keywords;                 /* keyword list — type keywords end with '|' */
	char *singleline_comment_start;  // e.g. "//"
	char *multiline_comment_start;   // e.g. "/*"
	char *multiline_comment_end;     // e.g. "*/"
	int flags;                       /* HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS */
};

/* Defined in syntax.c */
extern unsigned int HLDB_ENTRIES;

/*
 * One row of text in the file.
 * chars: heap-allocated string (null-terminated, but size doesn't count '\0')
 * hl:    parallel array of highlight types, one per character
 */
typedef struct {
	int size;
	char *chars;
	unsigned char *hl;      /* highlight type for each character */
	int hl_open_comment;    /* does this row end inside an unclosed multi-line comment? */
} erow;

/* All editor state lives here — one struct, easy to find, easy to refactor later */
struct EditorConfig {
	int cx, cy;         /* cursor position (column, row) — in FILE coordinates */
	int rowoff;         /* row offset — which file row is at the top of screen */
	int coloff;         /* col offset — which file col is at the left of screen */
	int screenrows;     /* terminal height */
	int screencols;     /* terminal width */
	int numrows;        /* how many rows of text are loaded */
	erow *row;          /* dynamic array of rows — grown with realloc */
	char *filename;     /* currently open file, NULL if new/empty */
	int dirty;          /* counts unsaved modifications, 0 = clean */
	char statusmsg[80]; /* message bar text (shown below status bar) */
	time_t statusmsg_time; /* when message was set — auto-expires after 5 seconds */
	struct editorSyntax *syntax;  /* current filetype syntax, NULL if none detected */
	int mouse_x, mouse_y;       /* last mouse click position (screen coordinates) */
	int sel_active;              /* 1 if text selection is active */
	int sel_anchor_x;            /* selection start column */
	int sel_anchor_y;            /* selection start row */
	char *clipboard;             /* internal clipboard (heap-allocated) */
	int clipboard_len;           /* length of clipboard content */
	int line_number_width;       /* width of line number gutter (digits + 1 space) */

	/* Undo/redo stacks — defined below */
	/* (forward-declared here, actual types defined right after EditorConfig) */
};

extern struct EditorConfig E;

extern struct termios orig_termios;

/* --- Undo/Redo system --- */

/*
 * Each undo entry type corresponds to one high-level edit action.
 * We record at the high level so one keypress = one undo entry.
 */
enum UndoType {
	UNDO_INSERT_CHAR,     /* user typed a character */
	UNDO_DELETE_CHAR,     /* backspace deleted a char (cx > 0) */
	UNDO_JOIN_LINES,      /* backspace at col 0 joined current line to previous */
	UNDO_INSERT_NEWLINE,  /* Enter split a line or inserted blank line */
	UNDO_DUPLICATE_LINE   /* Ctrl+D duplicated a line */
};

/*
 * Stores everything needed to reverse (or re-apply) one edit operation.
 * old_cx/old_cy = cursor BEFORE the edit, so we can restore it on undo.
 * str = heap-allocated copy of text data (for line join operations).
 */
typedef struct {
	enum UndoType type;
	int old_cx, old_cy;   /* cursor position before the edit */
	int c;                /* the character (for char insert/delete) */
	char *str;            /* saved text data — NULL if not needed */
	int str_len;          /* length of str */
	int indent;           /* auto-indent char count (for UNDO_INSERT_NEWLINE) */
} UndoEntry;

/*
 * Ring buffer stack — fixed size, auto-evicts oldest when full.
 * start = index of oldest entry, count = how many valid entries.
 * Push writes at (start + count) % UNDO_MAX.
 * Pop reads from (start + count - 1) % UNDO_MAX.
 */
typedef struct {
	UndoEntry entries[UNDO_MAX];
	int start;
	int count;
} UndoStack;

extern UndoStack undo_stack, redo_stack;

/*
 * We use enum values above 127 for special keys (arrows, etc.)
 * so they don't collide with normal ASCII characters (0-127).
 * This lets editorReadKey return a single int that represents
 * either a regular character OR a special key.
 */
enum editorKey {
	BACKSPACE = 127,    /* ASCII DEL — what Backspace sends in raw mode */
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,            /* \x1b[3~ — the Delete key */
	HOME_KEY,           /* \x1b[1~ or \x1b[H — jump to start of line */
	END_KEY,            /* \x1b[4~ or \x1b[F — jump to end of line */
	PAGE_UP,            /* \x1b[5~ — scroll up one page */
	PAGE_DOWN,          /* \x1b[6~ — scroll down one page */
	MOUSE_CLICK,        /* SGR mouse click event */
	SHIFT_LEFT,         /* \x1b[1;2D — Shift+Left for selection */
	SHIFT_RIGHT,        /* \x1b[1;2C — Shift+Right */
	SHIFT_UP,           /* \x1b[1;2A — Shift+Up */
	SHIFT_DOWN,         /* \x1b[1;2B — Shift+Down */
	ALT_BACKSPACE,      /* \x1b + DEL(127) — Option+Backspace for word delete */
	MOUSE_SCROLL_UP,    /* mouse wheel up (SGR button 64) */
	MOUSE_SCROLL_DOWN   /* mouse wheel down (SGR button 65) */
};

struct AppendBuffer {
    char *data;    // pointer to the heap-allocated memory
    int len;       // how many bytes are currently in the buffer
};

/* --- Function declarations --- */

/* terminal.c */
void disableRawMode(void);
void enableRawMode(void);
int getWindowSize(int *rows, int *cols);
int editorReadKey(void);

/* buffer.c */
void editorFreeRow(erow *row);
void editorInsertRow(int at, char *s, int len);
void editorAppendRow(char *s, int len);
void editorDeleteRow(int at);
void editorRowInsertChar(erow *row, int at, int c);
void editorRowDeleteChar(erow *row, int at);
void editorRowAppendString(erow *row, char *s, int len);

/* display.c */
void abAppend(struct AppendBuffer *ab, const char *s, int len);
void abFree(struct AppendBuffer *ab);
void editorUpdateLineNumberWidth(void);
void editorSetStatusMessage(const char *fmt, ...);
void editorDrawStatusBar(struct AppendBuffer *ab);
void editorDrawMessageBar(struct AppendBuffer *ab);
void editorScroll(void);
void editorRefreshScreen(void);

/* editor.c */
void undoStackInit(UndoStack *s);
void undoStackPush(UndoStack *s, UndoEntry entry);
int undoStackPop(UndoStack *s, UndoEntry *out);
void undoStackClear(UndoStack *s);
void editorInsertChar(int c);
void editorDeleteChar(void);
void editorInsertNewline(void);
void editorDuplicateLine(void);
void editorMoveCursor(int key);
void editorDeleteWord(void);
void editorAutoComplete(void);
void editorUndo(void);
void editorRedo(void);

/* file.c */
char *editorRowsToString(int *buflen);
char *editorPrompt(char *prompt, void (*callback)(char *, int));
void editorSave(void);
void editorOpen(char *filename);

/* search.c */
void editorFindCallback(char *query, int key);
void editorFind(void);

/* syntax.c */
int is_separator(int c);
int editorSyntaxToColor(int hl);
void editorUpdateSyntax(erow *row, int row_index);
void editorSelectSyntaxHighlight(void);

/* select.c */
void editorStartSelection(void);
void editorClearSelection(void);
int isSelected(int row, int col);
void editorCopySelection(void);
void editorPasteClipboard(void);

/* main.c */
void initEditor(void);
void editorProcessKeypress(void);

#endif /* ATLAS_H */
