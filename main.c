/* Atlas - terminal text editor */

#include <stdio.h>
#include <stdlib.h>    /* atexit() */
#include <unistd.h>    /* read(), STDIN_FILENO */
#include <termios.h>   /* struct termios, tcgetattr, tcsetattr */

/* Global: we need the original settings accessible from the atexit callback */
struct termios orig_termios;

/* Called automatically on exit — restores the terminal to its original state */
void disableRawMode(void)
{
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
}

int main(void)
{
	enableRawMode();

	/* Read keypresses and print their ASCII values */
	char c;
	while (1) {
		int nread = read(STDIN_FILENO, &c, 1);

		/* read() returns 0 on timeout (VMIN=0, VTIME=1), just loop again */
		if (nread == 0)
			continue;

		/* Quit on 'q' */
		if (c == 'q')
			break;

		/*
		 * Since OPOST is off, '\n' won't move cursor to start of line.
		 * We must print '\r\n' explicitly for proper newlines.
		 */
		if (c >= 32 && c < 127) {
			/* Printable character — show the char and its ASCII value */
			printf("'%c' (%d)\r\n", c, c);
		} else {
			/* Control character — just show the value */
			printf("(%d)\r\n", c);
		}
	}

	return 0;
}
