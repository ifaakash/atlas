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

/*
 * One row of text in the file.
 * chars: heap-allocated string (NOT null-terminated — we track length)
 * size:  number of bytes in chars
 */
typedef struct {
	int size;
	char *chars;
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
};

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
	PAGE_DOWN           /* \x1b[6~ — scroll down one page */
};

/* Forward declarations — needed when functions call others defined later in the file */
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
int editorReadKey(void);

/* Called automatically on exit — restores the terminal to its original state */
void disableRawMode(void)
{
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

/* --- Row operations --- */

/* Free memory owned by a row */
void editorFreeRow(erow *row)
{
	free(row->chars);
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
}

/* Delete character at position 'at' in a row */
void editorRowDeleteChar(erow *row, int at)
{
	if (at < 0 || at >= row->size) return;

	/* Shift characters left to close the gap — overwrites chars[at] */
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);

	row->size--;
	E.dirty++;
}

/* Append string 's' (length 'len') to end of a row */
void editorRowAppendString(erow *row, char *s, int len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	E.dirty++;
}

/* --- Editor-level operations (work on cursor position) --- */

/* Insert a character at the current cursor position */
void editorInsertChar(int c)
{
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

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		/* Normal case: delete character to the left */
		editorRowDeleteChar(row, E.cx - 1);
		E.cx--;
	} else {
		/* At column 0: join this line with the previous line */
		E.cx = E.row[E.cy - 1].size;  /* cursor goes to end of previous line */
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDeleteRow(E.cy);
		E.cy--;
	}
}

/* Insert a newline — splits the current row at the cursor position */
void editorInsertNewline(void)
{
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
	}

	E.cy++;
	E.cx = 0;
}

/*
 * Duplicate the current line (Ctrl+D).
 * Inserts a copy of the current row directly below, then moves cursor down.
 */
void editorDuplicateLine(void)
{
	if (E.cy >= E.numrows) return;  /* nothing to duplicate */

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

	/* Right side: current position */
	int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
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

	/* 3. Draw each row — file content if available, tilde if past end of file */
	int y;
	for (y = 0; y < E.screenrows; y++) {
		int filerow = y + E.rowoff;  /* map screen row to file row */

		if (filerow < E.numrows) {
			/* This row has file content — draw it, applying horizontal scroll */
			int len = E.row[filerow].size - E.coloff;
			if (len < 0) len = 0;            /* line is shorter than coloff */
			if (len > E.screencols)
				len = E.screencols;           /* truncate to screen width */
			if (len > 0)
				abAppend(&ab, &E.row[filerow].chars[E.coloff], len);
		} else {
			/* Past end of file — draw tilde */
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
	         (E.cx - E.coloff) + 1);
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
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			if (seq[1] >= '0' && seq[1] <= '9') {
				/* Extended escape: \x1b [ digit ~ */
				if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
				if (seq[2] == '~') {
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
	getWindowSize(&E.screenrows, &E.screencols);
	E.screenrows -= 2;  /* reserve 2 bottom rows for status bar + message bar */
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
			write(STDOUT_FILENO, "\x1b[2J", 4);   /* clear screen */
			write(STDOUT_FILENO, "\x1b[H", 3);    /* cursor home */
			exit(0);
			break;

		case CTRL_KEY('s'):                 /* Save */
			editorSave();
			break;

		case CTRL_KEY('d'):                 /* Duplicate line */
			editorDuplicateLine();
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
			editorMoveCursor(c);
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

	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find | Ctrl-D = dup line");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
