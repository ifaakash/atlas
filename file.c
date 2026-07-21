#include "atlas.h"

/* --- File I/O --- */

/*
 * Converts all editor rows into a single string with \n between lines.
 * Returns a malloc'd buffer — caller must free it.
 * Sets *buflen to the total length of the string.
 */
char *editorRowsToString(int *buflen)
{
	int totlen = 0;
	int j;

	/* Calculate total length: all row sizes + one \n per row */
	for (j = 0; j < E.numrows; j++)
		totlen += E.row[j].size + 1;

	*buflen = totlen;
	char *buf = malloc(totlen);
	char *p = buf;

	for (j = 0; j < E.numrows; j++) {
		memcpy(p, E.row[j].chars, E.row[j].size);
		p += E.row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

/*
 * Prompts the user for input in the message bar.
 * 'prompt' must contain a %s where the user's input will be shown.
 * 'callback' is called on every keypress (used by search for incremental results).
 * Pass NULL for callback if you just need simple text input.
 *
 * Returns: malloc'd string on Enter, or NULL if user pressed Escape.
 */
char *editorPrompt(char *prompt, void (*callback)(char *, int))
{
	size_t bufsize = 128;
	char *buf = malloc(bufsize);
	size_t buflen = 0;
	buf[0] = '\0';

	while (1) {
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c = editorReadKey();

		if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
			/* Delete last character from input */
			if (buflen != 0) buf[--buflen] = '\0';
		} else if (c == '\x1b') {
			/* Escape — cancel the prompt */
			editorSetStatusMessage("");
			if (callback) callback(buf, c);
			free(buf);
			return NULL;
		} else if (c == '\r') {
			/* Enter — confirm the input */
			if (buflen != 0) {
				editorSetStatusMessage("");
				if (callback) callback(buf, c);
				return buf;
			}
		} else if (!iscntrl(c) && c < 128) {
			/* Regular character — add to input buffer */
			if (buflen == bufsize - 1) {
				bufsize *= 2;
				buf = realloc(buf, bufsize);
			}
			buf[buflen++] = c;
			buf[buflen] = '\0';
		}

		if (callback) callback(buf, c);
	}
}

/*
 * Save the current buffer to disk.
 * If no filename is set, prompts for one.
 * Uses low-level open/write/close for precise error handling.
 *
 * O_RDWR    — open for reading and writing
 * O_CREAT   — create file if it doesn't exist
 * O_TRUNC   — truncate file to zero length if it exists
 * 0644      — file permissions: owner rw, group r, others r
 */
void editorSave(void)
{
	if (E.filename == NULL) {
		E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
		if (E.filename == NULL) {
			editorSetStatusMessage("Save aborted");
			return;
		}
		editorSelectSyntaxHighlight();  /* detect filetype from new filename */
	}

	int len;
	char *buf = editorRowsToString(&len);

	int fd = open(E.filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd != -1) {
		if (write(fd, buf, len) == len) {
			close(fd);
			free(buf);
			E.dirty = 0;
			editorSetStatusMessage("%d bytes written to disk", len);
			return;
		}
		close(fd);
	}

	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*
 * Opens a file and loads it line by line into E.row.
 *
 * Uses fopen/getline from <stdio.h>.
 * getline() reads one line at a time, allocating memory as needed.
 * We strip trailing \n and \r before storing — our renderer adds \r\n itself.
 */
void editorOpen(char *filename)
{
	free(E.filename);
	E.filename = strdup(filename);  /* store filename for status bar and saving */

	FILE *fp = fopen(filename, "r");
	if (!fp) return;

	char *line = NULL;
	size_t linecap = 0;  /* getline uses this to track allocated size */
	ssize_t linelen;

	/*
	 * getline() returns the number of chars read, or -1 at EOF.
	 * It allocates/reallocates `line` as needed — we must free it after.
	 * linecap tells getline how big the current buffer is.
	 */
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		/* Strip trailing newline/carriage return */
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
		                       line[linelen - 1] == '\r'))
			linelen--;

		editorAppendRow(line, linelen);
	}

	free(line);
	fclose(fp);
	E.dirty = 0;  /* just loaded — not modified yet */
	editorSelectSyntaxHighlight();
}
