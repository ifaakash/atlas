#include "atlas.h"

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
