#include "atlas.h"

/* Free memory owned by a row */
void editorFreeRow(erow *row)
{
	free(row->chars);
	free(row->hl);
}

/*
 * Insert a new row at position 'at' in the E.row array.
 * Shifts existing rows down using memmove.
 *
 * memmove is like memcpy but safe for overlapping memory regions.
 * When we shift rows down, source and destination overlap — so memmove is required.
 */
void editorInsertRow(int at, char *s, int len)
{
	if (at < 0 || at > E.numrows) return;

	/* Grow the array by one row */
	E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

	/* Shift rows after 'at' down by one to make room */
	memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

	/* Fill in the new row */
	E.row[at].size = len;
	E.row[at].chars = malloc(len + 1);  /* +1 for null terminator (needed by strstr in search) */
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].hl = NULL;
	E.row[at].hl_open_comment = 0;
	editorUpdateSyntax(&E.row[at], at);

	E.numrows++;
	E.dirty++;
}

/* Convenience wrapper — appends a row at the end (used during file loading) */
void editorAppendRow(char *s, int len)
{
	editorInsertRow(E.numrows, s, len);
	E.dirty = 0;  /* loading a file shouldn't count as dirty — reset after each append */
}

/* Delete row at index 'at' from E.row array */
void editorDeleteRow(int at)
{
	if (at < 0 || at >= E.numrows) return;
	editorFreeRow(&E.row[at]);

	/* Shift rows above 'at' up to fill the gap */
	memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
	E.numrows--;
	E.dirty++;
}

/*
 * Insert character 'c' at position 'at' in a row.
 * Uses memmove to shift existing characters right by one.
 */
void editorRowInsertChar(erow *row, int at, int c)
{
	if (at < 0 || at > row->size) at = row->size;

	/* +2: one for new char, one for null terminator */
	row->chars = realloc(row->chars, row->size + 2);

	/* Shift characters from 'at' onwards one position right */
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);

	row->size++;
	row->chars[at] = c;
	E.dirty++;
	editorUpdateSyntax(row, row - E.row);  /* pointer arithmetic gives row index */
}

/* Delete character at position 'at' in a row */
void editorRowDeleteChar(erow *row, int at)
{
	if (at < 0 || at >= row->size) return;

	/* Shift characters left to close the gap — overwrites chars[at] */
	memmove(&row->chars[at], &row->chars[at + 1], row->size - at);

	row->size--;
	E.dirty++;
	editorUpdateSyntax(row, row - E.row);
}

/* Append string 's' (length 'len') to end of a row */
void editorRowAppendString(erow *row, char *s, int len)
{
	row->chars = realloc(row->chars, row->size + len + 1);
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	E.dirty++;
	editorUpdateSyntax(row, row - E.row);
}
