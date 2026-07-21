/* Atlas - terminal text editor */

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

/* Global: we need the original settings accessible from the atexit callback */
struct termios orig_termios;

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

/* Flags for which syntax features a filetype supports */
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

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

/* --- Filetype database --- */

char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", NULL };

char *C_HL_keywords[] = {
	"switch", "if", "while", "for", "break", "continue", "return",
	"else", "struct", "union", "typedef", "static", "enum", "case",
	"sizeof", "#include", "#define", "do",
	/* Type keywords end with '|' — rendered in a different color */
	"int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
	"void|", "size_t|", "ssize_t|", "FILE|", NULL
};

/* HLDB: the highlight database. Add new languages here. */
struct editorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//", "/*", "*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

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

/* --- Undo/Redo system --- */

#define UNDO_MAX 1000

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

/* Now we can add the undo/redo fields — but since EditorConfig is already
 * defined above, we use a separate global. This avoids circular definition. */
UndoStack undo_stack;
UndoStack redo_stack;

struct EditorConfig E;

/* Ctrl key strips bits 5-7, leaving only bits 0-4. So Ctrl+Q = 'q' & 0x1f = 17 */
#define CTRL_KEY(k) ((k) & 0x1f)

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

/* Forward declarations — needed when functions call others defined later in the file */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
int editorReadKey(void);
void editorUpdateSyntax(erow *row, int row_index);
int isSelected(int row, int col);
void editorUpdateLineNumberWidth(void);

/* --- Undo stack operations --- */

void undoStackInit(UndoStack *s)
{
	s->start = 0;
	s->count = 0;
	memset(s->entries, 0, sizeof(s->entries));
}

/* Push an entry onto the stack. If full, evicts the oldest entry. */
void undoStackPush(UndoStack *s, UndoEntry entry)
{
	int idx;
	if (s->count == UNDO_MAX) {
		/* Stack full — free oldest entry's string and overwrite */
		free(s->entries[s->start].str);
		s->entries[s->start].str = NULL;
		idx = s->start;
		s->start = (s->start + 1) % UNDO_MAX;
	} else {
		idx = (s->start + s->count) % UNDO_MAX;
		s->count++;
	}
	/* Free any leftover string at this slot (defensive) */
	free(s->entries[idx].str);
	s->entries[idx] = entry;
}

/* Pop the most recent entry. Returns 1 on success, 0 if empty. */
int undoStackPop(UndoStack *s, UndoEntry *out)
{
	if (s->count == 0) return 0;
	s->count--;
	int idx = (s->start + s->count) % UNDO_MAX;
	*out = s->entries[idx];
	s->entries[idx].str = NULL;  /* prevent double-free */
	return 1;
}

/* Clear the stack and free all stored strings. */
void undoStackClear(UndoStack *s)
{
	int i;
	for (i = 0; i < s->count; i++) {
		int idx = (s->start + i) % UNDO_MAX;
		free(s->entries[idx].str);
		s->entries[idx].str = NULL;
	}
	s->start = 0;
	s->count = 0;
}

/* Called automatically on exit — restores the terminal to its original state */
void disableRawMode(void)
{
	write(STDOUT_FILENO, "\x1b[?1006l", 8);  /* disable SGR mouse format */
	write(STDOUT_FILENO, "\x1b[?1000l", 8);  /* disable mouse tracking */
	write(STDOUT_FILENO, "\x1b[?1049l", 8);  /* switch back to main screen buffer */
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

struct AppendBuffer {
    char *data;    // pointer to the heap-allocated memory
    int len;       // how many bytes are currently in the buffer
};

void abAppend(struct AppendBuffer *ab, const char *s, int len){
    // new data is appended to the ab variable, and then realloc is used to re-evalute the new size of variable
    ab->data = realloc(ab->data, ab->len + len);
    memcpy(ab->data + ab->len, s, len);
    ab->len += len;
}

/*
 * Gets the terminal dimensions (rows and columns).
 * Returns 0 on success, -1 on failure.
 *
 * Uses ioctl() — a system call for device-specific operations.
 * TIOCGWINSZ = "Terminal I/O Control Get WINdow SiZe"
 * It fills a struct winsize with .ws_row and .ws_col.
 *
 * We take pointers to rows/cols so the caller provides storage.
 * This is a common C pattern — functions "return" multiple values
 * by writing through pointers the caller passes in.
 */
int getWindowSize(int *rows, int *cols)
{
	struct winsize ws;

	/* ioctl returns -1 on failure, or ws_col/ws_row could be 0 (invalid) */
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		return -1;
	}

	*cols = ws.ws_col;
	*rows = ws.ws_row;
	return 0;
}

void abFree(struct AppendBuffer *ab){
	// this function just set len=0 or clear the data variable
	free(ab->data);
	ab->data = NULL;
	ab->len = 0;

}

/* --- Syntax highlighting --- */

/* Returns 1 if 'c' is a separator character (space, punctuation, or null) */
int is_separator(int c)
{
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];:!&|{}", c) != NULL;
}

/* Maps a highlight type to an ANSI color code (foreground colors 30-37) */
int editorSyntaxToColor(int hl)
{
	switch (hl) {
		case HL_NUMBER:     return 31;  /* red */
		case HL_MATCH:      return 34;  /* blue */
		case HL_STRING:     return 35;  /* magenta */
		case HL_COMMENT:
		case HL_MLCOMMENT:  return 36;  /* cyan */
		case HL_KEYWORD1:   return 33;  /* yellow */
		case HL_KEYWORD2:   return 32;  /* green */
		default:            return 37;  /* white (default) */
	}
}

/*
 * Updates the hl[] array for a single row — assigns a highlight type
 * to every character based on syntax rules.
 *
 * This is the most complex function in the editor. It walks through
 * each character and decides if it's part of a number, string, comment,
 * keyword, or just normal text.
 *
 * 'row_index' is needed to check the previous row's open comment state
 * and to cascade updates to the next row if our state changed.
 */
void editorUpdateSyntax(erow *row, int row_index)
{
	row->hl = realloc(row->hl, row->size);
	memset(row->hl, HL_NORMAL, row->size);

	if (E.syntax == NULL) return;

	char **keywords = E.syntax->keywords;
	char *scs = E.syntax->singleline_comment_start;
	char *mcs = E.syntax->multiline_comment_start;
	char *mce = E.syntax->multiline_comment_end;

	int scs_len = scs ? strlen(scs) : 0;
	int mcs_len = mcs ? strlen(mcs) : 0;
	int mce_len = mce ? strlen(mce) : 0;

	int prev_sep = 1;         /* previous char was a separator (start of line = yes) */
	int in_string = 0;        /* 0 = not in string, or the quote char if inside */
	int in_comment = (row_index > 0 && E.row[row_index - 1].hl_open_comment);

	int i = 0;
	while (i < row->size) {
		char c = row->chars[i];
		unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

		/* --- Single-line comments --- */
		if (scs_len && !in_string && !in_comment) {
			if (!strncmp(&row->chars[i], scs, scs_len)) {
				/* Rest of line is a comment */
				memset(&row->hl[i], HL_COMMENT, row->size - i);
				break;
			}
		}

		/* --- Multi-line comments --- */
		if (mcs_len && mce_len && !in_string) {
			if (in_comment) {
				row->hl[i] = HL_MLCOMMENT;
				if (!strncmp(&row->chars[i], mce, mce_len)) {
					memset(&row->hl[i], HL_MLCOMMENT, mce_len);
					i += mce_len;
					in_comment = 0;
					prev_sep = 1;
					continue;
				} else {
					i++;
					continue;
				}
			} else if (!strncmp(&row->chars[i], mcs, mcs_len)) {
				memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
				i += mcs_len;
				in_comment = 1;
				continue;
			}
		}

		/* --- Strings --- */
		if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				/* Handle backslash escapes (e.g., \" inside a string) */
				if (c == '\\' && i + 1 < row->size) {
					row->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string) in_string = 0;  /* closing quote */
				i++;
				prev_sep = 1;
				continue;
			} else {
				if (c == '"' || c == '\'') {
					in_string = c;
					row->hl[i] = HL_STRING;
					i++;
					continue;
				}
			}
		}

		/* --- Numbers --- */
		if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
			    (c == '.' && prev_hl == HL_NUMBER)) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		}

		/* --- Keywords --- */
		if (prev_sep) {
			int j;
			for (j = 0; keywords[j]; j++) {
				int klen = strlen(keywords[j]);
				/* Keywords ending with '|' are type keywords (HL_KEYWORD2) */
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2) klen--;

				/* Match keyword and ensure it's followed by a separator */
				if (!strncmp(&row->chars[i], keywords[j], klen) &&
				    is_separator(row->chars[i + klen])) {
					memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;
					break;
				}
			}
			if (keywords[j] != NULL) {
				prev_sep = 0;
				continue;
			}
		}

		prev_sep = is_separator(c);
		i++;
	}

	/* Track whether this row ends inside a multi-line comment */
	int changed = (row->hl_open_comment != in_comment);
	row->hl_open_comment = in_comment;

	/*
	 * If our open-comment state changed, the NEXT row's highlighting
	 * might be wrong — cascade the update. This handles multi-line
	 * comments that span many rows.
	 */
	if (changed && row_index + 1 < E.numrows)
		editorUpdateSyntax(&E.row[row_index + 1], row_index + 1);
}

/*
 * Detects the filetype from E.filename and sets E.syntax.
 * Checks each HLDB entry's filematch patterns against the filename extension.
 */
void editorSelectSyntaxHighlight(void)
{
	E.syntax = NULL;
	if (E.filename == NULL) return;

	/* Find the last '.' in the filename — that's the extension */
	char *ext = strrchr(E.filename, '.');

	unsigned int j;
	for (j = 0; j < HLDB_ENTRIES; j++) {
		struct editorSyntax *s = &HLDB[j];
		unsigned int i = 0;

		while (s->filematch[i]) {
			int is_ext = (s->filematch[i][0] == '.');

			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
			    (!is_ext && strstr(E.filename, s->filematch[i]))) {
				E.syntax = s;

				/* Re-highlight all rows with the new syntax */
				int filerow;
				for (filerow = 0; filerow < E.numrows; filerow++) {
					editorUpdateSyntax(&E.row[filerow], filerow);
				}
				return;
			}
			i++;
		}
	}
}

/* --- Row operations --- */

/* Free memory owned by a row */
void editorFreeRow(erow *row)
{
	free(row->chars);
	free(row->hl);
}

/*
 * Insert a new row at position 'at' in the E.row array.
 * Shifts existing rows down using memmove.
 *
 * memmove is like memcpy but safe for overlapping memory regions.
 * When we shift rows down, source and destination overlap — so memmove is required.
 */
void editorInsertRow(int at, char *s, int len)
{
	if (at < 0 || at > E.numrows) return;

	/* Grow the array by one row */
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	/* Shift rows after 'at' down by one to make room */
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

	/* Fill in the new row */
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);  /* +1 for null terminator (needed by strstr in search) */
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].hl = NULL;
	E.row[at].hl_open_comment = 0;
	editorUpdateSyntax(&E.row[at], at);

	E.numrows++;
	E.dirty++;
}

/* Convenience wrapper — appends a row at the end (used during file loading) */
void editorAppendRow(char *s, int len)
{
	editorInsertRow(E.numrows, s, len);
	E.dirty = 0;  /* loading a file shouldn't count as dirty — reset after each append */
}

/* Delete row at index 'at' from E.row array */
void editorDeleteRow(int at)
{
	if (at < 0 || at >= E.numrows) return;
	editorFreeRow(&E.row[at]);

	/* Shift rows above 'at' up to fill the gap */
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

/*
 * Insert character 'c' at position 'at' in a row.
 * Uses memmove to shift existing characters right by one.
 */
void editorRowInsertChar(erow *row, int at, int c)
{
	if (at < 0 || at > row->size) at = row->size;

	/* +2: one for new char, one for null terminator */
	row->chars = realloc(row->chars, row->size + 2);

	/* Shift characters from 'at' onwards one position right */
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);

	row->size++;
	row->chars[at] = c;
	E.dirty++;
	editorUpdateSyntax(row, row - E.row);  /* pointer arithmetic gives row index */
}

/* Delete character at position 'at' in a row */
void editorRowDeleteChar(erow *row, int at)
{
	if (at < 0 || at >= row->size) return;

	/* Shift characters left to close the gap — overwrites chars[at] */
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);

	row->size--;
	E.dirty++;
	editorUpdateSyntax(row, row - E.row);
}

/* Append string 's' (length 'len') to end of a row */
void editorRowAppendString(erow *row, char *s, int len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	E.dirty++;
	editorUpdateSyntax(row, row - E.row);
}

/* --- Editor-level operations (work on cursor position) --- */

/* Insert a character at the current cursor position */
void editorInsertChar(int c)
{
	/* Record undo BEFORE making the change */
	UndoEntry ue = {0};
	ue.type = UNDO_INSERT_CHAR;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;
	ue.c = c;
	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);  /* new edit kills redo history */

	/* If cursor is at the very end of the file, add a new empty row first */
	if (E.cy == E.numrows)
		editorInsertRow(E.numrows, "", 0);

	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

/*
 * Handle Backspace: delete the character before the cursor.
 * If at the start of a line, join with the previous line.
 */
void editorDeleteChar(void)
{
	if (E.cy == E.numrows) return;          /* past end of file */
	if (E.cx == 0 && E.cy == 0) return;    /* top-left corner, nothing to delete */

	UndoEntry ue = {0};
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		/* Normal case: delete character to the left */
		ue.type = UNDO_DELETE_CHAR;
		ue.c = row->chars[E.cx - 1];  /* save the char we're about to delete */
		undoStackPush(&undo_stack, ue);
		undoStackClear(&redo_stack);

		editorRowDeleteChar(row, E.cx - 1);
		E.cx--;
	} else {
		/* At column 0: join this line with the previous line */
		ue.type = UNDO_JOIN_LINES;
		ue.str = malloc(row->size);       /* save current row before it's destroyed */
		memcpy(ue.str, row->chars, row->size);
		ue.str_len = row->size;
		undoStackPush(&undo_stack, ue);
		undoStackClear(&redo_stack);

		E.cx = E.row[E.cy - 1].size;  /* cursor goes to end of previous line */
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDeleteRow(E.cy);
		E.cy--;
	}
}

/* Insert a newline — splits the current row at the cursor position */
void editorInsertNewline(void)
{
	/* Record undo — old_cx tells us the split point for reversal */
	UndoEntry ue = {0};
	ue.type = UNDO_INSERT_NEWLINE;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;
	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);

	if (E.cx == 0) {
		/* Cursor at start of line: insert a blank line above */
		editorInsertRow(E.cy, "", 0);
	} else {
		/* Split: text after cursor goes to new line below */
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);

		/* Truncate current row at cursor position */
		row = &E.row[E.cy];  /* re-read — realloc in editorInsertRow may have moved it */
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateSyntax(row, E.cy);
	}

	E.cy++;

	/*
	 * Auto-indent: copy leading whitespace from the previous line.
	 * Scans the line we just left for spaces/tabs at the beginning,
	 * then inserts them into the new line.
	 */
	erow *prev = &E.row[E.cy - 1];
	int indent = 0;
	while (indent < prev->size && (prev->chars[indent] == ' ' || prev->chars[indent] == '\t'))
		indent++;

	if (indent > 0) {
		int i;
		for (i = 0; i < indent; i++)
			editorRowInsertChar(&E.row[E.cy], i, prev->chars[i]);
		E.cx = indent;
	} else {
		E.cx = 0;
	}
}

/*
 * Duplicate the current line (Ctrl+D).
 * Inserts a copy of the current row directly below, then moves cursor down.
 */
void editorDuplicateLine(void)
{
	if (E.cy >= E.numrows) return;  /* nothing to duplicate */

	UndoEntry ue = {0};
	ue.type = UNDO_DUPLICATE_LINE;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;
	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);

	erow *row = &E.row[E.cy];
	editorInsertRow(E.cy + 1, row->chars, row->size);
	E.cy++;  /* move to the duplicated line */
	editorSetStatusMessage("Line duplicated");
}

/* --- File I/O --- */

/*
 * Converts all editor rows into a single string with \n between lines.
 * Returns a malloc'd buffer — caller must free it.
 * Sets *buflen to the total length of the string.
 */
char *editorRowsToString(int *buflen)
{
	int totlen = 0;
	int j;

	/* Calculate total length: all row sizes + one \n per row */
	for (j = 0; j < E.numrows; j++)
		totlen += E.row[j].size + 1;

	*buflen = totlen;
	char *buf = malloc(totlen);
	char *p = buf;

	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

/*
 * Prompts the user for input in the message bar.
 * 'prompt' must contain a %s where the user's input will be shown.
 * 'callback' is called on every keypress (used by search for incremental results).
 * Pass NULL for callback if you just need simple text input.
 *
 * Returns: malloc'd string on Enter, or NULL if user pressed Escape.
 */
char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
	size_t bufsize = 128;
	char *buf = malloc(bufsize);
	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();

		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			/* Delete last character from input */
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			/* Escape — cancel the prompt */
			editorSetStatusMessage("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			/* Enter — confirm the input */
			if (buflen != 0) {
				editorSetStatusMessage("");
				if (callback) callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			/* Regular character — add to input buffer */
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}

		if (callback) callback(buf, c);
	}
}

/*
 * Save the current buffer to disk.
 * If no filename is set, prompts for one.
 * Uses low-level open/write/close for precise error handling.
 *
 * O_RDWR    — open for reading and writing
 * O_CREAT   — create file if it doesn't exist
 * O_TRUNC   — truncate file to zero length if it exists
 * 0644      — file permissions: owner rw, group r, others r
 */
void editorSave(void)
{
	if (E.filename == NULL) {
		E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
		if (E.filename == NULL) {
			editorSetStatusMessage("Save aborted");
			return;
		}
		editorSelectSyntaxHighlight();  /* detect filetype from new filename */
	}

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd != -1) {
		if (write(fd, buf, len) == len) {
			close(fd);
			free(buf);
			E.dirty = 0;
			editorSetStatusMessage("%d bytes written to disk", len);
			return;
		}
		close(fd);
	}

	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/* --- Search --- */

/*
 * Callback called on every keypress during search.
 * Performs incremental search — results update as you type.
 *
 * Uses strstr to find the query in each row. Since our erow.chars
 * is null-terminated (+1 byte in editorInsertRow), strstr works directly.
 *
 * Arrow keys navigate between matches (forward/backward, wrapping around).
 */
void editorFindCallback(char *query, int key)
{
	/* Static variables persist across calls — track search state */
	static int last_match = -1;   /* row index of last match, -1 = none */
	static int direction = 1;     /* 1 = forward, -1 = backward */
	static int saved_hl_line;     /* which row had its hl modified */
	static char *saved_hl = NULL; /* backup of that row's hl array */

	/* Restore previously highlighted row to its original colors */
	if (saved_hl) {
		memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].size);
		free(saved_hl);
		saved_hl = NULL;
	}

	/* Enter or Escape — reset search state */
	if (key == '\r' || key == '\x1b') {
		last_match = -1;
		direction = 1;
		return;
	} else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
		direction = 1;
	} else if (key == ARROW_LEFT || key == ARROW_UP) {
		direction = -1;
	} else {
		/* New character typed — reset to search from beginning */
		last_match = -1;
		direction = 1;
	}

	if (query[0] == '\0') return;

	int current = last_match;
	int i;

	/* Search through all rows, wrapping around */
	for (i = 0; i < E.numrows; i++) {
		current += direction;
		if (current == -1) current = E.numrows - 1;    /* wrap backward */
		if (current == E.numrows) current = 0;          /* wrap forward */

		erow *row = &E.row[current];
		char *match = strstr(row->chars, query);

		if (match) {
			last_match = current;
			E.cy = current;
			E.cx = match - row->chars;  /* pointer arithmetic gives offset */
			E.rowoff = E.numrows;       /* force editorScroll to recenter */

			/* Save original hl, then set matched chars to HL_MATCH (blue) */
			saved_hl_line = current;
			saved_hl = malloc(row->size);
			memcpy(saved_hl, row->hl, row->size);
			memset(&row->hl[E.cx], HL_MATCH, strlen(query));
			break;
		}
	}
}

/*
 * Opens search mode (Ctrl+F).
 * Saves cursor position — restores it if search is cancelled with Escape.
 * Uses editorPrompt with editorFindCallback for incremental search.
 */
void editorFind(void)
{
	/* Save cursor position to restore on cancel */
	int saved_cx = E.cx;
	int saved_cy = E.cy;
	int saved_coloff = E.coloff;
	int saved_rowoff = E.rowoff;

	char *query = editorPrompt("Search: %s (ESC to cancel, Arrows to navigate)",
	                           editorFindCallback);

	if (query) {
		free(query);  /* search confirmed — stay at found position */
	} else {
		/* Cancelled — restore cursor */
		E.cx = saved_cx;
		E.cy = saved_cy;
		E.coloff = saved_coloff;
		E.rowoff = saved_rowoff;
	}
}

/*
 * Opens a file and loads it line by line into E.row.
 *
 * Uses fopen/getline from <stdio.h>.
 * getline() reads one line at a time, allocating memory as needed.
 * We strip trailing \n and \r before storing — our renderer adds \r\n itself.
 */
void editorOpen(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);  /* store filename for status bar and saving */

	FILE *fp = fopen(filename, "r");
	if (!fp) return;

	char *line = NULL;
	size_t linecap = 0;  /* getline uses this to track allocated size */
	ssize_t linelen;

	/*
	 * getline() returns the number of chars read, or -1 at EOF.
	 * It allocates/reallocates `line` as needed — we must free it after.
	 * linecap tells getline how big the current buffer is.
	 */
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		/* Strip trailing newline/carriage return */
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
		                       line[linelen - 1] == '\r'))
			linelen--;

		editorAppendRow(line, linelen);
	}

	free(line);
	fclose(fp);
	E.dirty = 0;  /* just loaded — not modified yet */
	editorSelectSyntaxHighlight();
}

/*
 * Sets a message to display in the message bar.
 * Uses printf-style format string with variadic arguments.
 * The message auto-expires after 5 seconds in editorDrawMessageBar.
 *
 * va_list/va_start/va_end: C mechanism for functions that accept
 * a variable number of arguments (like printf itself).
 * vsnprintf: like snprintf, but takes a va_list instead of ... args.
 */
void editorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/*
 * Draws the status bar — an inverted-color line at the bottom of the editor.
 *
 * Left side:  filename and line count
 * Right side: current line / total lines
 *
 * \x1b[7m = "reverse video" (swap foreground/background colors)
 * \x1b[m  = reset all formatting back to normal
 */
void editorDrawStatusBar(struct AppendBuffer *ab)
{
	abAppend(ab, "\x1b[7m", 4);  /* switch to inverted colors */

	char status[80], rstatus[80];

	/* Left side: filename + line count + dirty indicator */
	int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
	                   E.filename ? E.filename : "[No Name]",
	                   E.numrows,
	                   E.dirty ? "(modified)" : "");

	/* Right side: filetype and current position */
	int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
	                    E.syntax ? E.syntax->filetype : "no ft",
	                    E.cy + 1, E.numrows);

	if (len > E.screencols) len = E.screencols;
	abAppend(ab, status, len);

	/* Fill the rest of the bar with spaces (keeps the inverted background) */
	while (len < E.screencols) {
		if (E.screencols - len == rlen) {
			/* Right-align the position info */
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, " ", 1);
			len++;
		}
	}

	abAppend(ab, "\x1b[m", 3);  /* reset formatting */
	abAppend(ab, "\r\n", 2);    /* newline before message bar */
}

/*
 * Draws the message bar — one line below the status bar.
 * Shows E.statusmsg if it was set within the last 5 seconds.
 */
void editorDrawMessageBar(struct AppendBuffer *ab)
{
	abAppend(ab, "\x1b[K", 3);  /* clear this line */

	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;

	/* Only show message if it's less than 5 seconds old */
	if (msglen && time(NULL) - E.statusmsg_time < 5)
		abAppend(ab, E.statusmsg, msglen);
}

/*
 * Adjusts E.rowoff and E.coloff so the cursor is always visible on screen.
 * Called at the top of every screen refresh.
 *
 * Think of it like a camera following a character in a game:
 * the viewport shifts when the cursor reaches the edge.
 */
void editorScroll(void)
{
	/* Cursor moved above the visible area — scroll up */
	if (E.cy < E.rowoff)
		E.rowoff = E.cy;

	/* Cursor moved below the visible area — scroll down */
	if (E.cy >= E.rowoff + E.screenrows)
		E.rowoff = E.cy - E.screenrows + 1;

	/* Cursor moved left of the visible area — scroll left */
	if (E.cx < E.coloff)
		E.coloff = E.cx;

	/* Cursor moved right of the visible area — scroll right */
	if (E.cx >= E.coloff + E.screencols)
		E.coloff = E.cx - E.screencols + 1;
}

void editorRefreshScreen(void)
{
	editorScroll();

	struct AppendBuffer ab = {NULL, 0};

	/* 1. Hide cursor — prevents flickering during redraw */
	abAppend(&ab, "\x1b[?25l", 6);

	/* 2. Move cursor to top-left — we redraw from here */
	abAppend(&ab, "\x1b[H", 3);

	/* Update line number gutter width based on file size */
	editorUpdateLineNumberWidth();
	int content_cols = E.screencols - E.line_number_width;  /* columns available for text */

	/* 3. Draw each row — line number + file content or tilde */
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;  /* map screen row to file row */

		if (filerow < E.numrows) {
			/* Draw line number in dim color */
			char linenum[16];
			int lnlen = snprintf(linenum, sizeof(linenum), "%*d ",
			                     E.line_number_width - 1, filerow + 1);
			abAppend(&ab, "\x1b[90m", 5);  /* dim/gray color */
			abAppend(&ab, linenum, lnlen);
			abAppend(&ab, "\x1b[39m", 5);  /* reset color */

			/* Draw file content with syntax colors */
			int len = E.row[filerow].size - E.coloff;
			if (len < 0) len = 0;
			if (len > content_cols) len = content_cols;

			char *c = &E.row[filerow].chars[E.coloff];
			unsigned char *hl = &E.row[filerow].hl[E.coloff];
			int current_color = -1;  /* track current color to minimize escape sequences */
			int j;

			int in_sel = 0;  /* track if we're inside a selection highlight */
			for (j = 0; j < len; j++) {
				int col_idx = j + E.coloff;  /* actual file column */
				int sel = isSelected(filerow, col_idx);

				/* Toggle selection inversion on/off */
				if (sel && !in_sel) {
					abAppend(&ab, "\x1b[7m", 4);  /* inverted */
					in_sel = 1;
					current_color = -1;  /* force color re-emit inside selection */
				} else if (!sel && in_sel) {
					abAppend(&ab, "\x1b[m", 3);   /* reset all */
					in_sel = 0;
					current_color = -1;  /* force color re-emit after selection */
				}

				int color = editorSyntaxToColor(hl[j]);
				if (color != current_color) {
					current_color = color;
					char colbuf[16];
					int clen = snprintf(colbuf, sizeof(colbuf), "\x1b[%dm", color);
					abAppend(&ab, colbuf, clen);
				}
				abAppend(&ab, &c[j], 1);
			}
			if (in_sel) abAppend(&ab, "\x1b[m", 3);  /* reset if row ended in selection */
			abAppend(&ab, "\x1b[39m", 5);  /* reset foreground color */
		} else {
			/* Past end of file — draw tilde with gutter padding */
			char padding[16];
			int padlen = snprintf(padding, sizeof(padding), "%*s",
			                     E.line_number_width, "");
			abAppend(&ab, padding, padlen);
			abAppend(&ab, "~", 1);
		}

		/* Clear the rest of this line */
		abAppend(&ab, "\x1b[K", 3);

		/* Every row gets \r\n — including the last, because status bar follows */
		abAppend(&ab, "\r\n", 2);
	}

	/* 4. Draw status bar and message bar */
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	/*
	 * 5. Position cursor on screen.
	 * E.cy/E.cx are file coordinates. Subtract the scroll offset
	 * to get screen coordinates. Add 1 because terminal is 1-based.
	 */
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
	         (E.cy - E.rowoff) + 1,
	         (E.cx - E.coloff) + E.line_number_width + 1);  /* offset by gutter */
	abAppend(&ab, buf, strlen(buf));

	/* 5. Show cursor again */
	abAppend(&ab, "\x1b[?25h", 6);

	/* 6. Single write — everything hits the terminal at once, no flicker */
	write(STDOUT_FILENO, ab.data, ab.len);

	/* 7. Free the buffer */
	abFree(&ab);
}

/* Switches terminal to raw mode */
void enableRawMode(void)
{
	/* Save current terminal settings */
	tcgetattr(STDIN_FILENO, &orig_termios);

	/* Register restore function — runs on ANY exit (normal, error, etc.) */
	atexit(disableRawMode);

	struct termios raw = orig_termios;

	/*
	 * Input flags (c_iflag):
	 *   ICRNL  - terminal translates CR (Enter) to NL. Turn off so we see raw CR.
	 *   IXON   - Ctrl+S/Ctrl+Q pause/resume output. Useless for an editor.
	 */
	raw.c_iflag &= ~(ICRNL | IXON);

	/*
	 * Output flags (c_oflag):
	 *   OPOST  - terminal translates NL to CR+NL on output. We want raw output.
	 */
	raw.c_oflag &= ~(OPOST);

	/*
	 * Local flags (c_lflag):
	 *   ECHO   - stop terminal from printing what we type
	 *   ICANON - disable line buffering, read byte-by-byte
	 *   ISIG   - stop Ctrl+C (SIGINT) and Ctrl+Z (SIGTSTP) from generating signals
	 *   IEXTEN - disable Ctrl+V (literal next) on some systems
	 */
	raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

	/*
	 * Control characters:
	 *   VMIN = 0  - read() returns as soon as there's any input (or none)
	 *   VTIME = 1 - read() times out after 100ms if no input. Returns 0.
	 *              This lets us run a loop without blocking forever.
	 */
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	/* Apply the modified settings immediately */
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

	/*
	 * Alternate screen buffer: the terminal has two buffers — main (where
	 * your shell output lives) and alternate (for fullscreen apps like vim).
	 * Entering the alternate buffer means our editor output doesn't pollute
	 * the shell scrollback. When we exit, the main buffer is restored.
	 */
	write(STDOUT_FILENO, "\x1b[?1049h", 8);  /* switch to alternate screen buffer */
	write(STDOUT_FILENO, "\x1b[?1000h", 8);  /* enable mouse tracking (X11 mode) */
	write(STDOUT_FILENO, "\x1b[?1006h", 8);  /* enable SGR extended mouse format */
}

/*
 * Reads a single keypress and returns it.
 * Regular keys return their ASCII value.
 * Arrow keys return our enum values (1000+).
 *
 * Arrow keys send 3 bytes: \x1b [ A/B/C/D
 * We detect the \x1b, then read the next two bytes to identify which arrow.
 */
/*
 * Reads a single keypress and returns it.
 * Regular keys return their ASCII value.
 * Special keys (arrows, Home, End, etc.) return enum editorKey values.
 *
 * Escape sequences come in different formats:
 *   \x1b [ letter      — arrow keys: A/B/C/D, also H (Home), F (End)
 *   \x1b [ digit ~     — Delete(3), Home(1/7), End(4/8), PageUp(5), PageDown(6)
 */
int editorReadKey(void)
{
	char c;
	int nread;

	/* Keep trying until we actually get a byte */
	while ((nread = read(STDIN_FILENO, &c, 1)) == 0)
		;

	/* If it's an escape character, it might be a multi-byte sequence */
	if (c == '\x1b') {
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';

		/* Alt+Backspace: \x1b followed by DEL (127) — must check BEFORE reading seq[1] */
		if (seq[0] == '\x7f') return ALT_BACKSPACE;

		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			/*
			 * SGR mouse event: \x1b[< button;col;row M  (press) or m (release)
			 * Example: \x1b[<0;15;3M = left click at col 15, row 3
			 */
			if (seq[1] == '<') {
				char mousebuf[32];
				int mi = 0;
				/* Read until 'M' (press) or 'm' (release) */
				while (mi < (int)sizeof(mousebuf) - 1) {
					if (read(STDIN_FILENO, &mousebuf[mi], 1) != 1) break;
					if (mousebuf[mi] == 'M' || mousebuf[mi] == 'm') break;
					mi++;
				}
				mousebuf[mi + 1] = '\0';

				/* Parse button;col;row */
				int button = 0, col = 0, row = 0;
				sscanf(mousebuf, "%d;%d;%d", &button, &col, &row);

				/* Left-click press (button 0, ending with 'M') */
				if (button == 0 && mousebuf[mi] == 'M') {
					E.mouse_x = col - 1;  /* terminal is 1-based, we're 0-based */
					E.mouse_y = row - 1;
					return MOUSE_CLICK;
				}
				/* Mouse scroll: button 64 = scroll up, 65 = scroll down */
				if (button == 64) return MOUSE_SCROLL_UP;
				if (button == 65) return MOUSE_SCROLL_DOWN;
				return '\x1b';  /* ignore other mouse events */
			}

			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

				if (seq[2] == '~') {
					/* Extended escape: \x1b [ digit ~ */
					switch (seq[1]) {
						case '1': return HOME_KEY;
						case '3': return DEL_KEY;
						case '4': return END_KEY;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME_KEY;
						case '8': return END_KEY;
					}
				}

				/*
				 * Shift/Ctrl+Arrow: \x1b[1;2A = Shift+Up, \x1b[1;5C = Ctrl+Right
				 * seq[1]='1', seq[2]=';', then modifier digit, then direction letter
				 */
				if (seq[1] == '1' && seq[2] == ';') {
					char mod, dir;
					if (read(STDIN_FILENO, &mod, 1) != 1) return '\x1b';
					if (read(STDIN_FILENO, &dir, 1) != 1) return '\x1b';

					if (mod == '2') {
						/* Shift+Arrow — for selection */
						switch (dir) {
							case 'A': return SHIFT_UP;
							case 'B': return SHIFT_DOWN;
							case 'C': return SHIFT_RIGHT;
							case 'D': return SHIFT_LEFT;
						}
					}
					/* Other modifiers (Ctrl=5, Alt=3) — ignore for now */
				}
			} else {
				/* Simple escape: \x1b [ letter */
				switch (seq[1]) {
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME_KEY;
					case 'F': return END_KEY;
				}
			}
		} else if (seq[0] == 'O') {
			/* Some terminals send \x1b O H / \x1b O F for Home/End */
			switch (seq[1]) {
				case 'H': return HOME_KEY;
				case 'F': return END_KEY;
			}
		}

		return '\x1b';
	}

	return c;
}

/*
 * Updates cursor position based on which key was pressed.
 * Bounds checking is now against file content, not screen size.
 *
 * Also handles line wrapping:
 *   - Right arrow at end of line -> start of next line
 *   - Left arrow at start of line -> end of previous line
 *
 * After vertical movement, snaps cx to end of line if the
 * destination line is shorter than the current cx position.
 */
void editorMoveCursor(int key)
{
	/* Get the current row (NULL if cursor is past end of file) */
	erow *row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;

	switch (key) {
		case ARROW_LEFT:
			if (E.cx > 0) {
				E.cx--;
			} else if (E.cy > 0) {
				/* Wrap to end of previous line */
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size && E.cy < E.numrows - 1) {
				/* Wrap to start of next line */
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy > 0) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows - 1) E.cy++;
			break;
	}

	/* Snap cx to end of line if we moved to a shorter line */
	row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen)
		E.cx = rowlen;
}

/*
 * Initialize editor state — call once at startup.
 */
void initEditor(void)
{
	E.cx = 0;
	E.cy = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;
	E.dirty = 0;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.syntax = NULL;
	E.mouse_x = 0;
	E.mouse_y = 0;
	E.sel_active = 0;
	E.sel_anchor_x = 0;
	E.sel_anchor_y = 0;
	E.clipboard = NULL;
	E.clipboard_len = 0;
	E.line_number_width = 4;  /* default: 3 digits + 1 space */
	undoStackInit(&undo_stack);
	undoStackInit(&redo_stack);
	getWindowSize(&E.screenrows, &E.screencols);
	E.screenrows -= 2;  /* reserve 2 bottom rows for status bar + message bar */
}

/* --- Selection --- */

/* Start a selection if one isn't already active */
void editorStartSelection(void)
{
	if (!E.sel_active) {
		E.sel_anchor_x = E.cx;
		E.sel_anchor_y = E.cy;
		E.sel_active = 1;
	}
}

/* Clear the active selection */
void editorClearSelection(void)
{
	E.sel_active = 0;
}

/*
 * Check if a character at (row, col) is within the active selection.
 * Selection range is between anchor and cursor (either direction).
 *
 * Linearizes positions as (row * large_number + col) to compare.
 */
int isSelected(int row, int col)
{
	if (!E.sel_active) return 0;

	/* Compute linear positions for anchor and cursor */
	long anchor = (long)E.sel_anchor_y * 100000 + E.sel_anchor_x;
	long cursor = (long)E.cy * 100000 + E.cx;
	long pos = (long)row * 100000 + col;

	long start = anchor < cursor ? anchor : cursor;
	long end = anchor < cursor ? cursor : anchor;

	return pos >= start && pos < end;
}

/*
 * Copy selected text into E.clipboard.
 * Handles multi-line selections by joining with '\n'.
 */
void editorCopySelection(void)
{
	if (!E.sel_active) return;

	/* Determine selection bounds (start before end) */
	int sy, sx, ey, ex;
	if (E.sel_anchor_y < E.cy ||
	    (E.sel_anchor_y == E.cy && E.sel_anchor_x <= E.cx)) {
		sy = E.sel_anchor_y; sx = E.sel_anchor_x;
		ey = E.cy; ex = E.cx;
	} else {
		sy = E.cy; sx = E.cx;
		ey = E.sel_anchor_y; ex = E.sel_anchor_x;
	}

	/* Free old clipboard */
	free(E.clipboard);
	E.clipboard = NULL;
	E.clipboard_len = 0;

	/* Build clipboard content */
	struct AppendBuffer ab = {NULL, 0};
	int row;
	for (row = sy; row <= ey && row < E.numrows; row++) {
		int start_col = (row == sy) ? sx : 0;
		int end_col = (row == ey) ? ex : E.row[row].size;

		if (start_col > E.row[row].size) start_col = E.row[row].size;
		if (end_col > E.row[row].size) end_col = E.row[row].size;
		if (end_col > start_col)
			abAppend(&ab, &E.row[row].chars[start_col], end_col - start_col);

		if (row < ey)
			abAppend(&ab, "\n", 1);
	}

	E.clipboard = ab.data;
	E.clipboard_len = ab.len;
}

/* Paste clipboard content at cursor position */
void editorPasteClipboard(void)
{
	if (!E.clipboard || E.clipboard_len == 0) {
		editorSetStatusMessage("Clipboard empty");
		return;
	}

	int i;
	for (i = 0; i < E.clipboard_len; i++) {
		if (E.clipboard[i] == '\n') {
			editorInsertNewline();
		} else {
			editorInsertChar(E.clipboard[i]);
		}
	}
}

/* --- Word Delete --- */

/*
 * Delete backward one word (Option+Backspace / Alt+Backspace).
 * Deletes whitespace first, then deletes until the next word boundary.
 * Each char deletion goes through editorDeleteChar for undo support.
 */
void editorDeleteWord(void)
{
	if (E.cy >= E.numrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];

	/* If at column 0, just join with previous line (like normal backspace) */
	if (E.cx == 0) {
		editorDeleteChar();
		return;
	}

	/* Skip whitespace backward */
	while (E.cx > 0 && row->chars[E.cx - 1] == ' ')
		editorDeleteChar();

	/* Skip non-separator characters backward (the word itself) */
	while (E.cx > 0 && row->chars[E.cx - 1] != ' ')
		editorDeleteChar();
}

/* --- Line Numbers --- */

/*
 * Compute how wide the line number gutter should be.
 * Based on the number of lines in the file: 3 digits min, plus 1 space.
 * Example: 1-999 lines = "  1 " (4 cols), 1000+ = " 1000 " (6 cols).
 */
void editorUpdateLineNumberWidth(void)
{
	int digits = 1;
	int n = E.numrows;
	while (n >= 10) { n /= 10; digits++; }
	if (digits < 3) digits = 3;  /* minimum 3 digits wide */
	E.line_number_width = digits + 1;  /* +1 for the space separator */
}

/* --- Word Auto-Completion --- */

/*
 * Simple word completion from the current file.
 * Finds the partial word before the cursor, then scans all rows
 * for words starting with that prefix. Inserts the first match.
 *
 * Uses a basic approach: extracts the word under cursor, scans file
 * for matching words, cycles through matches on repeated presses.
 */
void editorAutoComplete(void)
{
	static int last_match_row = -1;
	static int last_match_col = -1;
	static int prefix_len_saved = 0;

	if (E.cy >= E.numrows || E.cx == 0) {
		editorSetStatusMessage("No word to complete");
		return;
	}

	erow *cur = &E.row[E.cy];

	/* Find the start of the current word (scan backward from cursor) */
	int word_start = E.cx;
	while (word_start > 0 && !isspace(cur->chars[word_start - 1]) &&
	       !strchr(",.()+-/*=~%<>[];:!&|{}\"'", cur->chars[word_start - 1]))
		word_start--;

	int prefix_len = E.cx - word_start;
	if (prefix_len == 0) {
		editorSetStatusMessage("No word to complete");
		return;
	}

	char *prefix = &cur->chars[word_start];

	/* If this is a fresh completion (not cycling), reset state */
	if (prefix_len != prefix_len_saved) {
		last_match_row = -1;
		last_match_col = -1;
		prefix_len_saved = prefix_len;
	}

	/* Scan all rows for a word matching the prefix */
	int start_row = (last_match_row >= 0) ? last_match_row : 0;
	int start_col = (last_match_col >= 0) ? last_match_col + 1 : 0;
	int found = 0;
	int r, pass;

	for (pass = 0; pass < 2 && !found; pass++) {
		for (r = (pass == 0) ? start_row : 0; r < E.numrows && !found; r++) {
			erow *row = &E.row[r];
			int c_start = (pass == 0 && r == start_row) ? start_col : 0;

			int c;
			for (c = c_start; c < row->size; c++) {
				/* Check if this position starts a word matching our prefix */
				if (c > 0 && !isspace(row->chars[c - 1]) &&
				    !strchr(",.()+-/*=~%<>[];:!&|{}\"'", row->chars[c - 1]))
					continue;  /* not at word boundary */

				/* Skip if this IS the word we're completing (same position) */
				if (r == E.cy && c == word_start)
					continue;

				/* Check prefix match */
				if (c + prefix_len > row->size)
					continue;
				if (strncmp(&row->chars[c], prefix, prefix_len) != 0)
					continue;

				/* Found a match — find end of the matched word */
				int word_end = c + prefix_len;
				while (word_end < row->size && !isspace(row->chars[word_end]) &&
				       !strchr(",.()+-/*=~%<>[];:!&|{}\"'", row->chars[word_end]))
					word_end++;

				/* Only complete if there's something beyond the prefix */
				if (word_end == c + prefix_len)
					continue;

				/* Insert the completion (chars after the prefix) */
				int comp_len = word_end - (c + prefix_len);
				int i;
				for (i = 0; i < comp_len; i++)
					editorInsertChar(row->chars[c + prefix_len + i]);

				last_match_row = r;
				last_match_col = c;
				prefix_len_saved = prefix_len + comp_len;

				editorSetStatusMessage("Completed: %.*s", word_end - c, &row->chars[c]);
				found = 1;
			}
		}
	}

	if (!found) {
		editorSetStatusMessage("No completion found");
		last_match_row = -1;
		last_match_col = -1;
	}
}

/* --- Undo/Redo --- */

/*
 * Undo the most recent edit operation.
 * Pops from undo_stack, reverses the operation using low-level primitives
 * (NOT the high-level functions, to avoid recording undo entries for undo itself),
 * then pushes a redo entry so the operation can be re-applied.
 */
void editorUndo(void)
{
	UndoEntry ue;
	if (!undoStackPop(&undo_stack, &ue)) {
		editorSetStatusMessage("Nothing to undo");
		return;
	}

	/* Build redo entry — same type and data, so redo can re-apply */
	UndoEntry re = {0};
	re.type = ue.type;
	re.old_cx = ue.old_cx;
	re.old_cy = ue.old_cy;
	re.c = ue.c;

	switch (ue.type) {
		case UNDO_INSERT_CHAR:
			/* Undo: remove the character that was inserted */
			editorRowDeleteChar(&E.row[ue.old_cy], ue.old_cx);
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_DELETE_CHAR:
			/* Undo: re-insert the character that was deleted */
			editorRowInsertChar(&E.row[ue.old_cy], ue.old_cx - 1, ue.c);
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_JOIN_LINES:
			/* Undo: split the joined line back into two */
			{
				/* The join appended ue.str to row[old_cy-1].
				 * To reverse: truncate row[old_cy-1] and insert row[old_cy] with ue.str. */
				int join_col = E.row[ue.old_cy - 1].size - ue.str_len;

				/* Insert the saved row text back */
				editorInsertRow(ue.old_cy, ue.str, ue.str_len);

				/* Truncate the previous row at the join point */
				E.row[ue.old_cy - 1].size = join_col;
				E.row[ue.old_cy - 1].chars[join_col] = '\0';
				editorUpdateSyntax(&E.row[ue.old_cy - 1], ue.old_cy - 1);

				/* Save the string for redo */
				re.str = malloc(ue.str_len);
				memcpy(re.str, ue.str, ue.str_len);
				re.str_len = ue.str_len;
			}
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_INSERT_NEWLINE:
			/* Undo: rejoin the two lines that were split */
			if (ue.old_cx == 0) {
				/* Was a blank line inserted above — just delete it */
				editorDeleteRow(ue.old_cy);
			} else {
				/* Rejoin: append row[old_cy+1] to row[old_cy], then delete row[old_cy+1] */
				editorRowAppendString(&E.row[ue.old_cy],
					E.row[ue.old_cy + 1].chars, E.row[ue.old_cy + 1].size);
				editorDeleteRow(ue.old_cy + 1);
			}
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_DUPLICATE_LINE:
			/* Undo: delete the duplicated row */
			editorDeleteRow(ue.old_cy + 1);
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;
	}

	free(ue.str);
	undoStackPush(&redo_stack, re);
}

/*
 * Redo the most recently undone operation.
 * Pops from redo_stack, re-applies the operation, pushes undo entry back.
 */
void editorRedo(void)
{
	UndoEntry re;
	if (!undoStackPop(&redo_stack, &re)) {
		editorSetStatusMessage("Nothing to redo");
		return;
	}

	/* Build undo entry so this can be undone again */
	UndoEntry ue = {0};
	ue.type = re.type;
	ue.old_cx = re.old_cx;
	ue.old_cy = re.old_cy;
	ue.c = re.c;

	switch (re.type) {
		case UNDO_INSERT_CHAR:
			/* Redo: re-insert the character */
			if (re.old_cy == E.numrows)
				editorInsertRow(E.numrows, "", 0);
			editorRowInsertChar(&E.row[re.old_cy], re.old_cx, re.c);
			E.cx = re.old_cx + 1;
			E.cy = re.old_cy;
			break;

		case UNDO_DELETE_CHAR:
			/* Redo: re-delete the character */
			ue.c = E.row[re.old_cy].chars[re.old_cx - 1];
			editorRowDeleteChar(&E.row[re.old_cy], re.old_cx - 1);
			E.cx = re.old_cx - 1;
			E.cy = re.old_cy;
			break;

		case UNDO_JOIN_LINES:
			/* Redo: re-join the lines (repeat the original backspace at col 0) */
			{
				erow *row = &E.row[re.old_cy];
				ue.str = malloc(row->size);
				memcpy(ue.str, row->chars, row->size);
				ue.str_len = row->size;

				E.cx = E.row[re.old_cy - 1].size;
				editorRowAppendString(&E.row[re.old_cy - 1], row->chars, row->size);
				editorDeleteRow(re.old_cy);
				E.cy = re.old_cy - 1;
			}
			break;

		case UNDO_INSERT_NEWLINE:
			/* Redo: re-split the line */
			if (re.old_cx == 0) {
				editorInsertRow(re.old_cy, "", 0);
			} else {
				erow *row = &E.row[re.old_cy];
				editorInsertRow(re.old_cy + 1, &row->chars[re.old_cx],
				               row->size - re.old_cx);
				row = &E.row[re.old_cy];  /* re-read after realloc */
				row->size = re.old_cx;
				row->chars[row->size] = '\0';
				editorUpdateSyntax(row, re.old_cy);
			}
			E.cy = re.old_cy + 1;
			E.cx = 0;
			break;

		case UNDO_DUPLICATE_LINE:
			/* Redo: re-duplicate the line */
			editorInsertRow(re.old_cy + 1,
				E.row[re.old_cy].chars, E.row[re.old_cy].size);
			E.cy = re.old_cy + 1;
			E.cx = re.old_cx;
			break;
	}

	free(re.str);
	undoStackPush(&undo_stack, ue);
}

/*
 * Central key handling — maps every keypress to an action.
 * This is the "controller" in the editor's main loop.
 */
void editorProcessKeypress(void)
{
	/*
	 * quit_times: safety counter for quitting with unsaved changes.
	 * Must press Ctrl+Q twice to force-quit a dirty buffer.
	 * Resets to 1 whenever any other key is pressed.
	 */
	static int quit_times = 1;

	int c = editorReadKey();

	switch (c) {
		case '\r':                          /* Enter key */
			editorInsertNewline();
			break;

		case CTRL_KEY('q'):                 /* Quit */
			if (E.dirty && quit_times > 0) {
				editorSetStatusMessage(
					"WARNING! Unsaved changes. Press Ctrl-Q again to quit.");
				quit_times--;
				return;  /* return early — don't reset quit_times below */
			}
			exit(0);  /* disableRawMode via atexit restores main screen buffer */
			break;

		case CTRL_KEY('s'):                 /* Save */
			editorSave();
			break;

		case CTRL_KEY('z'):                 /* Undo */
			editorUndo();
			break;

		case CTRL_KEY('y'):                 /* Redo */
			editorRedo();
			break;

		case CTRL_KEY('f'):                 /* Find/Search */
			editorFind();
			break;

		case CTRL_KEY('c'):                 /* Copy selection */
			if (E.sel_active) {
				editorCopySelection();
				editorClearSelection();
				editorSetStatusMessage("Copied to clipboard");
			}
			break;

		case CTRL_KEY('v'):                 /* Paste clipboard */
			editorClearSelection();
			editorPasteClipboard();
			break;

		case CTRL_KEY('d'):                 /* Duplicate line */
			editorClearSelection();
			editorDuplicateLine();
			break;

		case MOUSE_CLICK:                   /* Mouse click — position cursor */
			editorClearSelection();
			E.cy = E.mouse_y + E.rowoff;
			E.cx = (E.mouse_x - E.line_number_width) + E.coloff;
			if (E.cx < 0) E.cx = 0;  /* clicked in gutter */
			/* Clamp to file bounds */
			if (E.cy >= E.numrows)
				E.cy = E.numrows ? E.numrows - 1 : 0;
			if (E.cy < E.numrows && E.cx > E.row[E.cy].size)
				E.cx = E.row[E.cy].size;
			break;

		case SHIFT_UP:                      /* Shift+Arrow — extend selection */
		case SHIFT_DOWN:
		case SHIFT_LEFT:
		case SHIFT_RIGHT:
			editorStartSelection();
			/* Map shift keys to regular arrow movement */
			switch (c) {
				case SHIFT_UP:    editorMoveCursor(ARROW_UP); break;
				case SHIFT_DOWN:  editorMoveCursor(ARROW_DOWN); break;
				case SHIFT_LEFT:  editorMoveCursor(ARROW_LEFT); break;
				case SHIFT_RIGHT: editorMoveCursor(ARROW_RIGHT); break;
			}
			break;

		case ALT_BACKSPACE:                 /* Option+Backspace — delete word */
		case CTRL_KEY('w'):                 /* Ctrl+W — also delete word (universal) */
			editorClearSelection();
			editorDeleteWord();
			break;

		case MOUSE_SCROLL_UP:              /* Mouse wheel up — scroll 3 lines */
			{
				int i;
				for (i = 0; i < 3; i++) editorMoveCursor(ARROW_UP);
			}
			break;

		case MOUSE_SCROLL_DOWN:            /* Mouse wheel down — scroll 3 lines */
			{
				int i;
				for (i = 0; i < 3; i++) editorMoveCursor(ARROW_DOWN);
			}
			break;

		case CTRL_KEY('g'):                /* Go to line number */
			{
				char *input = editorPrompt("Go to line: %s", NULL);
				if (input) {
					int line = atoi(input);
					free(input);
					if (line > 0 && line <= E.numrows) {
						E.cy = line - 1;  /* user types 1-based, we're 0-based */
						E.cx = 0;
						E.rowoff = E.cy;  /* scroll so target line is at top */
						editorSetStatusMessage("Jumped to line %d", line);
					} else {
						editorSetStatusMessage("Invalid line number");
					}
				}
			}
			break;

		case BACKSPACE:                     /* Backspace key (127) */
		case CTRL_KEY('h'):                 /* Ctrl+H — historical backspace */
		case DEL_KEY:                       /* Delete key */
			if (c == DEL_KEY)
				editorMoveCursor(ARROW_RIGHT);  /* delete = backspace one position right */
			editorDeleteChar();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				/* Move cursor a full page up or down */
				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorClearSelection();
			editorMoveCursor(c);
			break;

		case '\t':                          /* Tab — auto-complete word */
			editorAutoComplete();
			break;

		case CTRL_KEY('l'):                 /* Refresh — no-op, redraws anyway */
		case '\x1b':                        /* Escape — ignore */
			break;

		default:
			editorInsertChar(c);            /* Normal character — insert it */
			break;
	}

	/* Any key other than Ctrl+Q resets the quit confirmation counter */
	quit_times = 1;
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();

	/* If a filename was passed on the command line, open it */
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("Ctrl-S=save | Ctrl-Q=quit | Ctrl-F=find | Ctrl-Z=undo | Ctrl-Y=redo");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
