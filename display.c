#include "atlas.h"

void abAppend(struct AppendBuffer *ab, const char *s, int len){
    ab->data = realloc(ab->data, ab->len + len);
    memcpy(ab->data + ab->len, s, len);
    ab->len += len;
}

void abFree(struct AppendBuffer *ab){
	free(ab->data);
	ab->data = NULL;
	ab->len = 0;
}

/* --- Line Numbers --- */

void editorUpdateLineNumberWidth(void)
{
	int digits = 1;
	int n = E.numrows;
	while (n >= 10) { n /= 10; digits++; }
	if (digits < 3) digits = 3;
	E.line_number_width = digits + 1;
}

/* --- Status Message --- */

void editorSetStatusMessage(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time = time(NULL);
}

/* --- Status Bar (redesigned with colors) --- */

void editorDrawStatusBar(struct AppendBuffer *ab)
{
	char status[256], rstatus[80];

	/* Left: ATLAS label + filename + modified indicator */
	int len = snprintf(status, sizeof(status), " ATLAS ");
	/* Blue background for the label */
	abAppend(ab, "\x1b[1m", 4);                /* bold */
	abAppend(ab, "\x1b[48;5;24m", 10);         /* dark blue bg */
	abAppend(ab, "\x1b[38;5;255m", 11);        /* white text */
	abAppend(ab, status, len);
	abAppend(ab, "\x1b[22m", 5);               /* unbold */

	/* Gray background for the rest */
	abAppend(ab, "\x1b[48;5;238m", 11);        /* dark gray bg */
	abAppend(ab, "\x1b[38;5;252m", 11);        /* light gray text */

	int flen = snprintf(status, sizeof(status), " %.40s %s",
	                    E.filename ? E.filename : "[untitled]",
	                    E.dirty ? "[+]" : "");
	if (flen > E.screencols - 7) flen = E.screencols - 7;
	abAppend(ab, status, flen);
	len += flen;

	/* Right: filetype | Ln X, Col Y | percentage */
	int pct = E.numrows > 0 ? ((E.cy + 1) * 100) / E.numrows : 0;
	int rlen = snprintf(rstatus, sizeof(rstatus), "%s | Ln %d, Col %d | %d%% ",
	                    E.syntax ? E.syntax->filetype : "text",
	                    E.cy + 1, E.cx + 1, pct);

	/* Fill with spaces between left and right */
	while (len < E.screencols) {
		if (E.screencols - len == rlen) {
			abAppend(ab, rstatus, rlen);
			break;
		} else {
			abAppend(ab, " ", 1);
			len++;
		}
	}

	abAppend(ab, "\x1b[m", 3);   /* reset all formatting */
	abAppend(ab, "\r\n", 2);
}

/* --- Message Bar --- */

void editorDrawMessageBar(struct AppendBuffer *ab)
{
	abAppend(ab, "\x1b[K", 3);

	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols) msglen = E.screencols;

	if (msglen && time(NULL) - E.statusmsg_time < 5) {
		abAppend(ab, "\x1b[38;5;243m", 11);  /* dim gray text */
		abAppend(ab, E.statusmsg, msglen);
		abAppend(ab, "\x1b[39m", 5);
	}
}

/* --- Scroll --- */

void editorScroll(void)
{
	if (E.cy < E.rowoff)
		E.rowoff = E.cy;

	if (E.cy >= E.rowoff + E.screenrows)
		E.rowoff = E.cy - E.screenrows + 1;

	int content_cols = E.screencols - E.line_number_width;

	if (E.cx < E.coloff)
		E.coloff = E.cx;

	if (E.cx >= E.coloff + content_cols)
		E.coloff = E.cx - content_cols + 1;
}

/* --- Welcome Screen --- */

static void editorDrawWelcomeScreen(struct AppendBuffer *ab)
{
	/* Welcome box content lines */
	const char *lines[] = {
		"\x1b[1m\x1b[38;5;75m   A T L A S\x1b[22m\x1b[38;5;243m",
		"\x1b[38;5;243m   terminal text editor",
		"",
		"\x1b[38;5;252m   Ctrl+Q  \x1b[38;5;243mquit",
		"\x1b[38;5;252m   Ctrl+S  \x1b[38;5;243msave",
		"\x1b[38;5;252m   Ctrl+F  \x1b[38;5;243mfind",
		"\x1b[38;5;252m   Ctrl+G  \x1b[38;5;243mgo to line",
		"\x1b[38;5;252m   Ctrl+Z  \x1b[38;5;243mundo",
		"\x1b[38;5;252m   Ctrl+D  \x1b[38;5;243mduplicate line",
		"\x1b[38;5;252m   Ctrl+W  \x1b[38;5;243mdelete word",
		"\x1b[38;5;252m   Tab     \x1b[38;5;243mauto-complete",
		"",
		"\x1b[38;5;240m   atlas <filename>",
	};
	int num_lines = sizeof(lines) / sizeof(lines[0]);

	/* Vertical centering */
	int start_row = (E.screenrows - num_lines) / 2;
	if (start_row < 0) start_row = 0;

	int y;
	for (y = 0; y < E.screenrows; y++) {
		int line_idx = y - start_row;

		if (line_idx >= 0 && line_idx < num_lines) {
			/* Center horizontally — pad with spaces (use ~30 char visual width) */
			int padding = (E.screencols - 30) / 2;
			if (padding < 0) padding = 0;
			int p;
			for (p = 0; p < padding; p++)
				abAppend(ab, " ", 1);
			abAppend(ab, lines[line_idx], strlen(lines[line_idx]));
			abAppend(ab, "\x1b[m", 3);  /* reset after each line */
		}

		abAppend(ab, "\x1b[K", 3);
		abAppend(ab, "\r\n", 2);
	}
}

/* --- Screen Refresh --- */

void editorRefreshScreen(void)
{
	editorScroll();

	struct AppendBuffer ab = {NULL, 0};

	/* Hide cursor during redraw */
	abAppend(&ab, "\x1b[?25l", 6);
	abAppend(&ab, "\x1b[H", 3);

	editorUpdateLineNumberWidth();
	int content_cols = E.screencols - E.line_number_width;

	/* Welcome screen when no file is loaded */
	if (E.numrows == 0 && E.filename == NULL) {
		editorDrawWelcomeScreen(&ab);
	} else {
		/* Draw each row */
		int y;
		for (y = 0; y < E.screenrows; y++) {
			int filerow = y + E.rowoff;
			int is_current_line = (filerow == E.cy);

			if (filerow < E.numrows) {
				/* Current line highlight — slightly lighter background */
				if (is_current_line)
					abAppend(&ab, "\x1b[48;5;237m", 11);

				/* Line number */
				char linenum[16];
				int lnlen = snprintf(linenum, sizeof(linenum), "%*d ",
				                     E.line_number_width - 1, filerow + 1);
				if (is_current_line)
					abAppend(&ab, "\x1b[38;5;255m", 11);  /* bright white for current line */
				else
					abAppend(&ab, "\x1b[38;5;240m", 11);  /* dim gray for other lines */
				abAppend(&ab, linenum, lnlen);
				abAppend(&ab, "\x1b[39m", 5);  /* reset fg */

				/* File content with syntax colors */
				int len = E.row[filerow].size - E.coloff;
				if (len < 0) len = 0;
				if (len > content_cols) len = content_cols;

				char *c = &E.row[filerow].chars[E.coloff];
				unsigned char *hl = &E.row[filerow].hl[E.coloff];
				int current_color = -1;
				int j;

				int in_sel = 0;
				for (j = 0; j < len; j++) {
					int col_idx = j + E.coloff;
					int sel = isSelected(filerow, col_idx);

					if (sel && !in_sel) {
						abAppend(&ab, "\x1b[48;5;24m", 10);  /* blue selection bg */
						in_sel = 1;
						current_color = -1;
					} else if (!sel && in_sel) {
						if (is_current_line)
							abAppend(&ab, "\x1b[48;5;237m", 11);  /* restore line highlight */
						else
							abAppend(&ab, "\x1b[49m", 5);  /* default bg */
						in_sel = 0;
						current_color = -1;
					}

					int color = editorSyntaxToColor(hl[j]);
					if (color != current_color) {
						current_color = color;
						char colbuf[16];
						int clen = snprintf(colbuf, sizeof(colbuf),
						                    "\x1b[38;5;%dm", color);
						abAppend(&ab, colbuf, clen);
					}
					abAppend(&ab, &c[j], 1);
				}
				if (in_sel) abAppend(&ab, "\x1b[49m", 5);

				/* Reset all formatting at end of row */
				abAppend(&ab, "\x1b[m", 3);
			} else {
				/* Past end of file — empty */
			}

			abAppend(&ab, "\x1b[K", 3);
			abAppend(&ab, "\r\n", 2);
		}
	}

	/* Status bar and message bar */
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	/* Position cursor */
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
	         (E.cy - E.rowoff) + 1,
	         (E.cx - E.coloff) + E.line_number_width + 1);
	abAppend(&ab, buf, strlen(buf));

	/* Show cursor */
	abAppend(&ab, "\x1b[?25h", 6);

	write(STDOUT_FILENO, ab.data, ab.len);
	abFree(&ab);
}
