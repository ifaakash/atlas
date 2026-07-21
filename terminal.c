#include "atlas.h"

/* Global: we need the original settings accessible from the atexit callback */
struct termios orig_termios;

/* Called automatically on exit — restores the terminal to its original state */
void disableRawMode(void)
{
	write(STDOUT_FILENO, "\x1b[?1006l", 8);  /* disable SGR mouse format */
	write(STDOUT_FILENO, "\x1b[?1000l", 8);  /* disable mouse tracking */
	write(STDOUT_FILENO, "\x1b[?1049l", 8);  /* switch back to main screen buffer */
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
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
