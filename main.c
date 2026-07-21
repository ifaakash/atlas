/* Atlas - terminal text editor */

#include <stdio.h>
#include <stdlib.h>    /* atexit() */
#include <unistd.h>    /* read(), STDIN_FILENO */
#include <termios.h>   /* struct termios, tcgetattr, tcsetattr */
#include <string.h>    /* memcpy */
#include <sys/ioctl.h> /* ioctl, TIOCGWINSZ, struct winsize */

/* Global: we need the original settings accessible from the atexit callback */
struct termios orig_termios;

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
	int rows, cols;
	getWindowSize(&rows, &cols);

	struct AppendBuffer ab = {NULL, 0};

	/* 1. Hide cursor — prevents flickering during redraw */
	abAppend(&ab, "\x1b[?25l", 6);

	/* 2. Move cursor to top-left — we redraw from here */
	abAppend(&ab, "\x1b[H", 3);

	/* 3. Draw tildes on every row, like vim's empty-file display */
	int y;
	for (y = 0; y < rows; y++) {
		abAppend(&ab, "~", 1);

		/* Clear the rest of this line (erases leftover chars from previous frame) */
		abAppend(&ab, "\x1b[K", 3);

		/* Don't add newline on the very last row — avoids scrolling the screen */
		if (y < rows - 1) {
			abAppend(&ab, "\r\n", 2);
		}
	}

	/* 4. Move cursor back to top-left */
	abAppend(&ab, "\x1b[H", 3);

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

int main(void)
{
	enableRawMode();

	while (1) {
		/* Refresh screen every loop iteration */
		editorRefreshScreen();

		/* Read a keypress */
		char c;
		int nread = read(STDIN_FILENO, &c, 1);

		if (nread == 0)
			continue;

		/* Quit on Ctrl+Q */
		if (c == CTRL_KEY('q'))
			break;
	}

	return 0;
}
