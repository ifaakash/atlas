#include "atlas.h"

void abAppend(struct AppendBuffer *ab, const char *s, int len){
    // new data is appended to the ab variable, and then realloc is used to re-evalute the new size of variable
    ab->data = realloc(ab->data, ab->len + len);
    memcpy(ab->data + ab->len, s, len);
    ab->len += len;
}

void abFree(struct AppendBuffer *ab){
	// this function just set len=0 or clear the data variable
	free(ab->data);
	ab->data = NULL;
	ab->len = 0;

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
			/* Past end of file — empty line (just gutter space) */
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
