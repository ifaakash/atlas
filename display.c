#include "atlas.h"

/*
 * Check if position (filerow, col_idx) is part of a non-selected
 * occurrence of E.highlight_word. Returns 1 if should be highlighted.
 */
static int isOccurrenceHighlight(int filerow, int col_idx)
{
	if (!E.highlight_word || E.highlight_word_len == 0) return 0;
	if (filerow >= E.numrows) return 0;

	erow *row = &E.row[filerow];
	int wlen = E.highlight_word_len;

	/* Check each possible start position that could include col_idx */
	int check_start = col_idx - wlen + 1;
	if (check_start < 0) check_start = 0;

	int s;
	for (s = check_start; s <= col_idx; s++) {
		if (s + wlen > row->size) continue;
		if (strncmp(&row->chars[s], E.highlight_word, wlen) != 0) continue;

		/* Check word boundaries */
		int left_ok = (s == 0 || is_separator(row->chars[s - 1]));
		int right_ok = (s + wlen >= row->size || is_separator(row->chars[s + wlen]));
		if (!left_ok || !right_ok) continue;

		/* Skip if this IS the selected instance */
		if (E.sel_active && E.sel_anchor_y == E.cy) {
			int sel_start = E.sel_anchor_x < E.cx ? E.sel_anchor_x : E.cx;
			if (filerow == E.sel_anchor_y && s == sel_start) continue;
		}

		return 1;
	}
	return 0;
}

/*
 * Check if position matches any multi-cursor insertion point.
 */
static int isMultiCursorPos(int filerow, int col_idx)
{
	if (!E.multi_active) return 0;
	int i;
	for (i = 0; i < E.multi_count; i++) {
		if (E.multi_cursors[i].row == filerow &&
		    col_idx == E.multi_cursors[i].col + E.multi_typed_len)
			return 1;
	}
	return 0;
}

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
	if (!E.show_line_numbers) {
		E.line_number_width = 0;
		return;
	}
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
		"\x1b[38;5;252m   Ctrl+K  \x1b[38;5;243mdelete line",
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

	/* Update occurrence highlight word from selection */
	free(E.highlight_word);
	E.highlight_word = NULL;
	E.highlight_word_len = 0;
	if (E.sel_active && E.sel_anchor_y == E.cy && E.cy < E.numrows) {
		int start = E.sel_anchor_x < E.cx ? E.sel_anchor_x : E.cx;
		int end = E.sel_anchor_x < E.cx ? E.cx : E.sel_anchor_x;
		int wlen = end - start;
		if (wlen > 0 && end <= E.row[E.cy].size) {
			int left_ok = (start == 0 || is_separator(E.row[E.cy].chars[start - 1]));
			int right_ok = (end >= E.row[E.cy].size || is_separator(E.row[E.cy].chars[end]));
			int has_sep = 0;
			int i;
			for (i = start; i < end; i++) {
				if (is_separator(E.row[E.cy].chars[i])) { has_sep = 1; break; }
			}
			if (left_ok && right_ok && !has_sep) {
				E.highlight_word = malloc(wlen + 1);
				memcpy(E.highlight_word, &E.row[E.cy].chars[start], wlen);
				E.highlight_word[wlen] = '\0';
				E.highlight_word_len = wlen;
			}
		}
	}

	/* Compute bracket match before drawing */
	E.bracket_match_row = -1;
	E.bracket_match_col = -1;
	if (E.cy < E.numrows && E.cx < E.row[E.cy].size) {
		editorFindMatchingBracket(E.cy, E.cx,
			&E.bracket_match_row, &E.bracket_match_col);
	}

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
				if (E.show_line_numbers) {
					char linenum[16];
					int lnlen = snprintf(linenum, sizeof(linenum), "%*d ",
					                     E.line_number_width - 1, filerow + 1);
					if (is_current_line)
						abAppend(&ab, "\x1b[38;5;255m", 11);  /* bright white for current line */
					else
						abAppend(&ab, "\x1b[38;5;240m", 11);  /* dim gray for other lines */
					abAppend(&ab, linenum, lnlen);
					abAppend(&ab, "\x1b[39m", 5);  /* reset fg */
				}

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

					/* Occurrence highlight (subtle gray bg for matching words) */
					int is_occurrence = (!sel && isOccurrenceHighlight(filerow, col_idx));
					if (is_occurrence && !in_sel) {
						abAppend(&ab, "\x1b[48;5;239m", 10);  /* subtle gray bg */
						current_color = -1;
					}

					/* Multi-cursor indicator (underline at cursor positions) */
					int is_multi = isMultiCursorPos(filerow, col_idx);
					if (is_multi) {
						abAppend(&ab, "\x1b[4m", 4);  /* underline on */
					}

					/* Bracket match highlight */
					int is_bracket_match =
						(filerow == E.bracket_match_row &&
						 col_idx == E.bracket_match_col);

					if (is_bracket_match) {
						abAppend(&ab, "\x1b[48;5;58m", 10);  /* dark yellow bg */
						abAppend(&ab, "\x1b[38;5;226m", 11); /* bright yellow fg */
						current_color = -1;
					}

					int color = editorSyntaxToColor(hl[j]);
					if (!is_bracket_match && color != current_color) {
						current_color = color;
						char colbuf[16];
						int clen = snprintf(colbuf, sizeof(colbuf),
						                    "\x1b[38;5;%dm", color);
						abAppend(&ab, colbuf, clen);
					}
					abAppend(&ab, &c[j], 1);

					if (is_multi) {
						abAppend(&ab, "\x1b[24m", 5);  /* underline off */
					}

					if (is_occurrence && !in_sel) {
						if (is_current_line)
							abAppend(&ab, "\x1b[48;5;237m", 11);
						else
							abAppend(&ab, "\x1b[49m", 5);
						current_color = -1;
					}

					if (is_bracket_match) {
						if (in_sel)
							abAppend(&ab, "\x1b[48;5;24m", 10);
						else if (is_current_line)
							abAppend(&ab, "\x1b[48;5;237m", 11);
						else
							abAppend(&ab, "\x1b[49m", 5);
						current_color = -1;
					}
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
