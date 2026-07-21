#include "atlas.h"

/* --- Selection --- */

/* Start a selection if one isn't already active */
void editorStartSelection(void)
{
	if (!E.sel_active) {
		E.sel_anchor_x = E.cx;
		E.sel_anchor_y = E.cy;
		E.sel_active = 1;
	}
}

/* Clear the active selection */
void editorClearSelection(void)
{
	E.sel_active = 0;
}

/*
 * Check if a character at (row, col) is within the active selection.
 * Selection range is between anchor and cursor (either direction).
 *
 * Linearizes positions as (row * large_number + col) to compare.
 */
int isSelected(int row, int col)
{
	if (!E.sel_active) return 0;

	/* Compute linear positions for anchor and cursor */
	long anchor = (long)E.sel_anchor_y * 100000 + E.sel_anchor_x;
	long cursor = (long)E.cy * 100000 + E.cx;
	long pos = (long)row * 100000 + col;

	long start = anchor < cursor ? anchor : cursor;
	long end = anchor < cursor ? cursor : anchor;

	return pos >= start && pos < end;
}

/*
 * Copy selected text into E.clipboard.
 * Handles multi-line selections by joining with '\n'.
 */
void editorCopySelection(void)
{
	if (!E.sel_active) return;

	/* Determine selection bounds (start before end) */
	int sy, sx, ey, ex;
	if (E.sel_anchor_y < E.cy ||
	    (E.sel_anchor_y == E.cy && E.sel_anchor_x <= E.cx)) {
		sy = E.sel_anchor_y; sx = E.sel_anchor_x;
		ey = E.cy; ex = E.cx;
	} else {
		sy = E.cy; sx = E.cx;
		ey = E.sel_anchor_y; ex = E.sel_anchor_x;
	}

	/* Free old clipboard */
	free(E.clipboard);
	E.clipboard = NULL;
	E.clipboard_len = 0;

	/* Build clipboard content */
	struct AppendBuffer ab = {NULL, 0};
	int row;
	for (row = sy; row <= ey && row < E.numrows; row++) {
		int start_col = (row == sy) ? sx : 0;
		int end_col = (row == ey) ? ex : E.row[row].size;

		if (start_col > E.row[row].size) start_col = E.row[row].size;
		if (end_col > E.row[row].size) end_col = E.row[row].size;
		if (end_col > start_col)
			abAppend(&ab, &E.row[row].chars[start_col], end_col - start_col);

		if (row < ey)
			abAppend(&ab, "\n", 1);
	}

	E.clipboard = ab.data;
	E.clipboard_len = ab.len;
}

/* Paste clipboard content at cursor position */
void editorPasteClipboard(void)
{
	if (!E.clipboard || E.clipboard_len == 0) {
		editorSetStatusMessage("Clipboard empty");
		return;
	}

	int i;
	for (i = 0; i < E.clipboard_len; i++) {
		if (E.clipboard[i] == '\n') {
			editorInsertNewline();
		} else {
			editorInsertChar(E.clipboard[i]);
		}
	}
}
