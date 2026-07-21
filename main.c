/* Atlas - terminal text editor */

#include <stdio.h>
#include <stdlib.h>    /* atexit() */
#include <unistd.h>    /* read(), STDIN_FILENO */
#include <termios.h>   /* struct termios, tcgetattr, tcsetattr */
#include <string.h>    /* memcpy */
#include <sys/ioctl.h> /* ioctl, TIOCGWINSZ, struct winsize */

/* Global: we need the original settings accessible from the atexit callback */
struct termios orig_termios;

/* All editor state lives here — one struct, easy to find, easy to refactor later */
struct EditorConfig {
	int cx, cy;         /* cursor position (column, row) */
	int screenrows;     /* terminal height */
	int screencols;     /* terminal width */
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

void editorRefreshScreen(void)
{
	struct AppendBuffer ab = {NULL, 0};

	/* 1. Hide cursor — prevents flickering during redraw */
	abAppend(&ab, "\x1b[?25l", 6);

	/* 2. Move cursor to top-left — we redraw from here */
	abAppend(&ab, "\x1b[H", 3);

	/* 3. Draw tildes on every row, like vim's empty-file display */
	int y;
	for (y = 0; y < E.screenrows; y++) {
		abAppend(&ab, "~", 1);

		/* Clear the rest of this line (erases leftover chars from previous frame) */
		abAppend(&ab, "\x1b[K", 3);

		/* Don't add newline on the very last row — avoids scrolling the screen */
		if (y < E.screenrows - 1) {
			abAppend(&ab, "\r\n", 2);
		}
	}

	/*
	 * 4. Position cursor at E.cy, E.cx
	 * Escape sequence is \x1b[{row};{col}H — but terminal rows/cols are 1-based,
	 * while our E.cx/E.cy are 0-based, so we add 1 to each.
	 * snprintf writes formatted text into a buffer — like printf but to a string.
	 */
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
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
 * Bounds checking: don't let the cursor go off-screen.
 */
void editorMoveCursor(int key)
{
	switch (key) {
		case ARROW_LEFT:
			if (E.cx > 0) E.cx--;
			break;
		case ARROW_RIGHT:
			if (E.cx < E.screencols - 1) E.cx++;
			break;
		case ARROW_UP:
			if (E.cy > 0) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.screenrows - 1) E.cy++;
			break;
	}
}

/*
 * Initialize editor state — call once at startup.
 */
void initEditor(void)
{
	E.cx = 0;
	E.cy = 0;
	getWindowSize(&E.screenrows, &E.screencols);
}

int main(void)
{
	enableRawMode();
	initEditor();

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
