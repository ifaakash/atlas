/* Atlas - terminal text editor */

#include <stdio.h>
#include <stdlib.h>    /* atexit() */
#include <unistd.h>    /* read(), STDIN_FILENO */
#include <termios.h>   /* struct termios, tcgetattr, tcsetattr */
#include <string.h>    /* memcpy, strdup */
#include <sys/ioctl.h> /* ioctl, TIOCGWINSZ, struct winsize */
#include <time.h>      /* time, time_t */
#include <stdarg.h>    /* va_list, va_start, va_end for variadic functions */

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

/*
 * Appends a new row to E.row array.
 * - realloc grows the array by one erow
 * - malloc allocates space for the line's characters
 * - memcpy copies the line content into that space
 *
 * This is the same grow-and-copy pattern as abAppend,
 * but for an array of structs instead of raw bytes.
 */
void editorAppendRow(char *s, int len)
{
	/* Grow the row array by one */
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len);
	memcpy(E.row[at].chars, s, len);

	E.numrows++;
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

/* Ctrl key strips bits 5-7, leaving only bits 0-4. So Ctrl+Q = 'q' & 0x1f = 17 */
#define CTRL_KEY(k) ((k) & 0x1f)

/*
 * We use enum values above 127 for special keys (arrows, etc.)
 * so they don't collide with normal ASCII characters (0-127).
 * This lets editorReadKey return a single int that represents
 * either a regular character OR a special key.
 */
enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,    /* 1001 — enum auto-increments */
	ARROW_UP,       /* 1002 */
	ARROW_DOWN      /* 1003 */
};

/*
 * Reads a single keypress and returns it.
 * Regular keys return their ASCII value.
 * Arrow keys return our enum values (1000+).
 *
 * Arrow keys send 3 bytes: \x1b [ A/B/C/D
 * We detect the \x1b, then read the next two bytes to identify which arrow.
 */
int editorReadKey(void)
{
	char c;
	int nread;

	/* Keep trying until we actually get a byte */
	while ((nread = read(STDIN_FILENO, &c, 1)) == 0)
		;

	/* If it's an escape character, it might be an arrow key sequence */
	if (c == '\x1b') {
		char seq[2];

		/*
		 * Try to read the next two bytes. If they don't arrive
		 * (timeout), the user just pressed Escape by itself.
		 */
		if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

		if (seq[0] == '[') {
			switch (seq[1]) {
				case 'A': return ARROW_UP;
				case 'B': return ARROW_DOWN;
				case 'C': return ARROW_RIGHT;
				case 'D': return ARROW_LEFT;
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

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();

	/* If a filename was passed on the command line, open it */
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("HELP: Ctrl-Q = quit");

	while (1) {
		editorRefreshScreen();

		int key = editorReadKey();

		/* Quit on Ctrl+Q */
		if (key == CTRL_KEY('q'))
			break;

		editorMoveCursor(key);
	}

	return 0;
}
