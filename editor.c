#include "atlas.h"

/* Now we can add the undo/redo fields — but since EditorConfig is already
 * defined above, we use a separate global. This avoids circular definition. */
UndoStack undo_stack;
UndoStack redo_stack;

/* --- Undo stack operations --- */

/* Peek at the top entry without popping. Returns NULL if empty. */
UndoEntry *undoStackPeek(UndoStack *s)
{
	if (s->count == 0) return NULL;
	int idx = (s->start + s->count - 1) % UNDO_MAX;
	return &s->entries[idx];
}

void undoStackInit(UndoStack *s)
{
	s->start = 0;
	s->count = 0;
	memset(s->entries, 0, sizeof(s->entries));
}

/* Push an entry onto the stack. If full, evicts the oldest entry. */
void undoStackPush(UndoStack *s, UndoEntry entry)
{
	int idx;
	if (s->count == UNDO_MAX) {
		/* Stack full — free oldest entry's string and overwrite */
		free(s->entries[s->start].str);
		s->entries[s->start].str = NULL;
		idx = s->start;
		s->start = (s->start + 1) % UNDO_MAX;
	} else {
		idx = (s->start + s->count) % UNDO_MAX;
		s->count++;
	}
	/* Free any leftover string at this slot (defensive) */
	free(s->entries[idx].str);
	s->entries[idx] = entry;
}

/* Pop the most recent entry. Returns 1 on success, 0 if empty. */
int undoStackPop(UndoStack *s, UndoEntry *out)
{
	if (s->count == 0) return 0;
	s->count--;
	int idx = (s->start + s->count) % UNDO_MAX;
	*out = s->entries[idx];
	s->entries[idx].str = NULL;  /* prevent double-free */
	return 1;
}

/* Clear the stack and free all stored strings. */
void undoStackClear(UndoStack *s)
{
	int i;
	for (i = 0; i < s->count; i++) {
		int idx = (s->start + i) % UNDO_MAX;
		free(s->entries[idx].str);
		s->entries[idx].str = NULL;
	}
	s->start = 0;
	s->count = 0;
}

/* --- Editor-level operations (work on cursor position) --- */

/* Insert a character at the current cursor position.
 * Uses word-boundary undo grouping: consecutive non-separator chars
 * accumulate into a single UNDO_INSERT_STRING entry.
 * Separators (space, punctuation) each get their own entry. */
void editorInsertChar(int c)
{
	int sep = is_separator(c);

	/* Try to append to the current undo group if:
	 * - char is NOT a separator
	 * - top of undo stack is UNDO_INSERT_STRING on the same row
	 * - cursor is right after the grouped text (contiguous typing) */
	if (!sep) {
		UndoEntry *top = undoStackPeek(&undo_stack);
		if (top && top->type == UNDO_INSERT_STRING &&
		    top->old_cy == E.cy &&
		    (top->old_cx + top->str_len) == E.cx) {
			/* Extend the existing group */
			top->str = realloc(top->str, top->str_len + 1);
			top->str[top->str_len] = (char)c;
			top->str_len++;
			/* Don't clear redo — already cleared when group started */
			goto do_insert;
		}
	}

	/* Start a new undo entry (new group or separator) */
	{
		UndoEntry ue = {0};
		ue.type = UNDO_INSERT_STRING;
		ue.old_cx = E.cx;
		ue.old_cy = E.cy;
		ue.str = malloc(1);
		ue.str[0] = (char)c;
		ue.str_len = 1;
		undoStackPush(&undo_stack, ue);
		undoStackClear(&redo_stack);
	}

do_insert:
	/* If cursor is at the very end of the file, add a new empty row first */
	if (E.cy == E.numrows)
		editorInsertRow(E.numrows, "", 0);

	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

/*
 * Handle Backspace: delete the character before the cursor.
 * If at the start of a line, join with the previous line.
 */
void editorDeleteChar(void)
{
	if (E.cy == E.numrows) return;          /* past end of file */
	if (E.cx == 0 && E.cy == 0) return;    /* top-left corner, nothing to delete */

	UndoEntry ue = {0};
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;

	erow *row = &E.row[E.cy];
	if (E.cx > 0) {
		/* Normal case: delete character to the left */
		ue.type = UNDO_DELETE_CHAR;
		ue.c = row->chars[E.cx - 1];  /* save the char we're about to delete */
		undoStackPush(&undo_stack, ue);
		undoStackClear(&redo_stack);

		editorRowDeleteChar(row, E.cx - 1);
		E.cx--;
	} else {
		/* At column 0: join this line with the previous line */
		ue.type = UNDO_JOIN_LINES;
		ue.str = malloc(row->size);       /* save current row before it's destroyed */
		memcpy(ue.str, row->chars, row->size);
		ue.str_len = row->size;
		undoStackPush(&undo_stack, ue);
		undoStackClear(&redo_stack);

		E.cx = E.row[E.cy - 1].size;  /* cursor goes to end of previous line */
		editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
		editorDeleteRow(E.cy);
		E.cy--;
	}
}

/* Insert a newline — splits the current row at the cursor position */
void editorInsertNewline(void)
{
	/* Save cursor BEFORE the operation for undo */
	UndoEntry ue = {0};
	ue.type = UNDO_INSERT_NEWLINE;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;

	if (E.cx == 0) {
		/* Cursor at start of line: insert a blank line above */
		editorInsertRow(E.cy, "", 0);
	} else {
		/* Split: text after cursor goes to new line below */
		erow *row = &E.row[E.cy];
		editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);

		/* Truncate current row at cursor position */
		row = &E.row[E.cy];  /* re-read — realloc in editorInsertRow may have moved it */
		row->size = E.cx;
		row->chars[row->size] = '\0';
		editorUpdateSyntax(row, E.cy);
	}

	E.cy++;

	/*
	 * Auto-indent: copy leading whitespace from the previous line.
	 * We count the indent BEFORE inserting so undo knows how many chars to strip.
	 */
	int indent = 0;
	if (E.auto_indent) {
		erow *prev = &E.row[E.cy - 1];
		while (indent < prev->size && (prev->chars[indent] == ' ' || prev->chars[indent] == '\t'))
			indent++;

		if (indent > 0) {
			int i;
			for (i = 0; i < indent; i++)
				editorRowInsertChar(&E.row[E.cy], i, prev->chars[i]);
			E.cx = indent;
		} else {
			E.cx = 0;
		}
	} else {
		E.cx = 0;
	}

	/* Record undo AFTER auto-indent so indent count is captured */
	ue.indent = indent;
	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);
}

/*
 * Duplicate the current line (Ctrl+D).
 * Inserts a copy of the current row directly below, then moves cursor down.
 */
void editorDuplicateLine(void)
{
	if (E.cy >= E.numrows) return;  /* nothing to duplicate */

	UndoEntry ue = {0};
	ue.type = UNDO_DUPLICATE_LINE;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;
	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);

	erow *row = &E.row[E.cy];
	editorInsertRow(E.cy + 1, row->chars, row->size);
	E.cy++;  /* move to the duplicated line */
	editorSetStatusMessage("Line duplicated");
}

/*
 * Delete the entire current line (Ctrl+K).
 * Saves the full line content for undo.
 */
void editorDeleteLine(void)
{
	if (E.numrows == 0) return;
	if (E.cy >= E.numrows) return;

	UndoEntry ue = {0};
	ue.type = UNDO_DELETE_LINE;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;

	/* Save the line content for undo restoration */
	erow *row = &E.row[E.cy];
	ue.str = malloc(row->size);
	memcpy(ue.str, row->chars, row->size);
	ue.str_len = row->size;
	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);

	editorDeleteRow(E.cy);

	/* Clamp cursor to valid position */
	if (E.cy >= E.numrows && E.numrows > 0)
		E.cy = E.numrows - 1;
	if (E.numrows > 0 && E.cx > E.row[E.cy].size)
		E.cx = E.row[E.cy].size;

	editorSetStatusMessage("Line deleted");
}

/*
 * Updates cursor position based on which key was pressed.
 * Bounds checking is now against file content, not screen size.
 *
 * Also handles line wrapping:
 *   - Right arrow at end of line -> start of next line
 *   - Left arrow at start of line -> end of previous line
 *
 * After vertical movement, snaps cx to end of line if the
 * destination line is shorter than the current cx position.
 */
void editorMoveCursor(int key)
{
	/* Get the current row (NULL if cursor is past end of file) */
	erow *row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;

	switch (key) {
		case ARROW_LEFT:
			if (E.cx > 0) {
				E.cx--;
			} else if (E.cy > 0) {
				/* Wrap to end of previous line */
				E.cy--;
				E.cx = E.row[E.cy].size;
			}
			break;
		case ARROW_RIGHT:
			if (row && E.cx < row->size) {
				E.cx++;
			} else if (row && E.cx == row->size && E.cy < E.numrows - 1) {
				/* Wrap to start of next line */
				E.cy++;
				E.cx = 0;
			}
			break;
		case ARROW_UP:
			if (E.cy > 0) E.cy--;
			break;
		case ARROW_DOWN:
			if (E.cy < E.numrows - 1) E.cy++;
			break;
	}

	/* Snap cx to end of line if we moved to a shorter line */
	row = (E.cy < E.numrows) ? &E.row[E.cy] : NULL;
	int rowlen = row ? row->size : 0;
	if (E.cx > rowlen)
		E.cx = rowlen;
}

/*
 * Move cursor left by one word.
 * Skips whitespace first, then skips the word (non-whitespace/non-separator chars).
 * Stops at the beginning of the word.
 */
void editorMoveWordLeft(void)
{
	if (E.cy >= E.numrows) return;

	/* If at start of line, jump to end of previous line */
	if (E.cx == 0) {
		if (E.cy > 0) {
			E.cy--;
			E.cx = E.row[E.cy].size;
		}
		return;
	}

	erow *row = &E.row[E.cy];

	/* Skip whitespace backward */
	while (E.cx > 0 && isspace(row->chars[E.cx - 1]))
		E.cx--;

	/* Skip word characters backward */
	while (E.cx > 0 && !isspace(row->chars[E.cx - 1]) &&
	       !strchr(",.()+-/*=~%<>[];:!&|{}\"'", row->chars[E.cx - 1]))
		E.cx--;
}

/*
 * Move cursor right by one word.
 * Skips the current word first, then skips whitespace.
 * Stops at the beginning of the next word.
 */
void editorMoveWordRight(void)
{
	if (E.cy >= E.numrows) return;

	erow *row = &E.row[E.cy];

	/* If at end of line, jump to start of next line */
	if (E.cx >= row->size) {
		if (E.cy < E.numrows - 1) {
			E.cy++;
			E.cx = 0;
		}
		return;
	}

	/* Skip word characters forward */
	while (E.cx < row->size && !isspace(row->chars[E.cx]) &&
	       !strchr(",.()+-/*=~%<>[];:!&|{}\"'", row->chars[E.cx]))
		E.cx++;

	/* Skip whitespace forward */
	while (E.cx < row->size && isspace(row->chars[E.cx]))
		E.cx++;
}

/* --- Word Delete --- */

/*
 * Delete backward one word (Option+Backspace / Alt+Backspace).
 * Deletes whitespace first, then deletes until the next word boundary.
 * Each char deletion goes through editorDeleteChar for undo support.
 */
void editorDeleteWord(void)
{
	if (E.cy >= E.numrows) return;
	if (E.cx == 0 && E.cy == 0) return;

	erow *row = &E.row[E.cy];

	/* If at column 0, just join with previous line (like normal backspace) */
	if (E.cx == 0) {
		editorDeleteChar();
		return;
	}

	/* Skip whitespace backward */
	while (E.cx > 0 && row->chars[E.cx - 1] == ' ')
		editorDeleteChar();

	/* Skip non-separator characters backward (the word itself) */
	while (E.cx > 0 && row->chars[E.cx - 1] != ' ')
		editorDeleteChar();
}

/* --- Word Auto-Completion --- */

/*
 * Simple word completion from the current file.
 * Finds the partial word before the cursor, then scans all rows
 * for words starting with that prefix. Inserts the first match.
 *
 * Uses a basic approach: extracts the word under cursor, scans file
 * for matching words, cycles through matches on repeated presses.
 */
void editorAutoComplete(void)
{
	static int last_match_row = -1;
	static int last_match_col = -1;
	static int prefix_len_saved = 0;

	if (E.cy >= E.numrows || E.cx == 0) {
		editorSetStatusMessage("No word to complete");
		return;
	}

	erow *cur = &E.row[E.cy];

	/* Find the start of the current word (scan backward from cursor) */
	int word_start = E.cx;
	while (word_start > 0 && !isspace(cur->chars[word_start - 1]) &&
	       !strchr(",.()+-/*=~%<>[];:!&|{}\"'", cur->chars[word_start - 1]))
		word_start--;

	int prefix_len = E.cx - word_start;
	if (prefix_len == 0) {
		editorSetStatusMessage("No word to complete");
		return;
	}

	char *prefix = &cur->chars[word_start];

	/* If this is a fresh completion (not cycling), reset state */
	if (prefix_len != prefix_len_saved) {
		last_match_row = -1;
		last_match_col = -1;
		prefix_len_saved = prefix_len;
	}

	/* Scan all rows for a word matching the prefix */
	int start_row = (last_match_row >= 0) ? last_match_row : 0;
	int start_col = (last_match_col >= 0) ? last_match_col + 1 : 0;
	int found = 0;
	int r, pass;

	for (pass = 0; pass < 2 && !found; pass++) {
		for (r = (pass == 0) ? start_row : 0; r < E.numrows && !found; r++) {
			erow *row = &E.row[r];
			int c_start = (pass == 0 && r == start_row) ? start_col : 0;

			int c;
			for (c = c_start; c < row->size; c++) {
				/* Check if this position starts a word matching our prefix */
				if (c > 0 && !isspace(row->chars[c - 1]) &&
				    !strchr(",.()+-/*=~%<>[];:!&|{}\"'", row->chars[c - 1]))
					continue;  /* not at word boundary */

				/* Skip if this IS the word we're completing (same position) */
				if (r == E.cy && c == word_start)
					continue;

				/* Check prefix match */
				if (c + prefix_len > row->size)
					continue;
				if (strncmp(&row->chars[c], prefix, prefix_len) != 0)
					continue;

				/* Found a match — find end of the matched word */
				int word_end = c + prefix_len;
				while (word_end < row->size && !isspace(row->chars[word_end]) &&
				       !strchr(",.()+-/*=~%<>[];:!&|{}\"'", row->chars[word_end]))
					word_end++;

				/* Only complete if there's something beyond the prefix */
				if (word_end == c + prefix_len)
					continue;

				/* Insert the completion (chars after the prefix) */
				int comp_len = word_end - (c + prefix_len);
				int i;
				for (i = 0; i < comp_len; i++)
					editorInsertChar(row->chars[c + prefix_len + i]);

				last_match_row = r;
				last_match_col = c;
				prefix_len_saved = prefix_len + comp_len;

				editorSetStatusMessage("Completed: %.*s", word_end - c, &row->chars[c]);
				found = 1;
			}
		}
	}

	if (!found) {
		editorSetStatusMessage("No completion found");
		last_match_row = -1;
		last_match_col = -1;
	}
}

/* --- Bracket Matching --- */

/*
 * Find the matching bracket for the character at (row, col).
 * Returns 1 if found (fills match_row/match_col), 0 if not.
 * Skips characters inside strings and comments.
 * Caps scan at 1000 lines for performance.
 */
int editorFindMatchingBracket(int row, int col, int *match_row, int *match_col)
{
	if (row >= E.numrows || col >= E.row[row].size) return 0;

	char ch = E.row[row].chars[col];
	char target;
	int direction;

	switch (ch) {
		case '(': target = ')'; direction = 1; break;
		case ')': target = '('; direction = -1; break;
		case '{': target = '}'; direction = 1; break;
		case '}': target = '{'; direction = -1; break;
		case '[': target = ']'; direction = 1; break;
		case ']': target = '['; direction = -1; break;
		default: return 0;
	}

	int depth = 1;
	int r = row, c = col;
	int max_lines = 1000;

	while (depth > 0) {
		/* Advance position */
		c += direction;
		while (c < 0 || (r < E.numrows && c >= E.row[r].size)) {
			r += direction;
			if (r < 0 || r >= E.numrows) return 0;
			if (abs(r - row) > max_lines) return 0;
			c = (direction > 0) ? 0 : E.row[r].size - 1;
			if (E.row[r].size == 0) {
				c = (direction > 0) ? 0 : -1;
				continue;
			}
		}
		if (r < 0 || r >= E.numrows) return 0;
		if (c < 0 || c >= E.row[r].size) continue;

		/* Skip chars inside strings/comments */
		unsigned char hl = E.row[r].hl[c];
		if (hl == HL_STRING || hl == HL_COMMENT || hl == HL_MLCOMMENT)
			continue;

		char cur = E.row[r].chars[c];
		if (cur == ch) depth++;
		else if (cur == target) depth--;
	}

	*match_row = r;
	*match_col = c;
	return 1;
}

/* --- Multi-Cursor Replace --- */

/*
 * Recalculate multi-cursor column positions after typing.
 * Each cursor on the same row as an earlier cursor shifts by multi_typed_len
 * per earlier same-row cursor (since those cursors inserted text before it).
 */
static void editorMultiCursorRecalcCols(void)
{
	int i, j;
	for (i = 0; i < E.multi_count; i++) {
		int same_row_before = 0;
		for (j = 0; j < i; j++) {
			if (E.multi_cursors[j].row == E.multi_cursors[i].row)
				same_row_before++;
		}
		E.multi_cursors[i].col = E.multi_cursors[i].base_col +
		                          same_row_before * E.multi_typed_len;
	}
}

/*
 * Enter multi-cursor mode: find all occurrences of the selected word,
 * delete the original word at each position, place cursors.
 */
void editorMultiCursorStart(void)
{
	if (!E.highlight_word || E.highlight_word_len == 0) return;

	/* Save the original word */
	E.multi_original = strdup(E.highlight_word);
	E.multi_original_len = E.highlight_word_len;
	E.multi_typed_len = 0;

	/* Find ALL whole-word occurrences in the file */
	E.multi_count = 0;
	int r;
	for (r = 0; r < E.numrows && E.multi_count < MAX_MULTI_CURSORS; r++) {
		erow *row = &E.row[r];
		int c = 0;
		while (c + E.multi_original_len <= row->size) {
			if (strncmp(&row->chars[c], E.multi_original, E.multi_original_len) == 0) {
				int left_ok = (c == 0 || is_separator(row->chars[c - 1]));
				int right_ok = (c + E.multi_original_len >= row->size ||
				               is_separator(row->chars[c + E.multi_original_len]));
				if (left_ok && right_ok) {
					E.multi_cursors[E.multi_count].row = r;
					E.multi_cursors[E.multi_count].col = c;
					E.multi_count++;
					c += E.multi_original_len;
					continue;
				}
			}
			c++;
		}
	}

	if (E.multi_count == 0) {
		free(E.multi_original);
		E.multi_original = NULL;
		editorSetStatusMessage("No occurrences found");
		return;
	}

	/* Delete the original word at each position (end to start) */
	int i;
	for (i = E.multi_count - 1; i >= 0; i--) {
		editorRowDeleteChars(&E.row[E.multi_cursors[i].row],
		                     E.multi_cursors[i].col,
		                     E.multi_original_len);
	}

	/* Adjust cursor columns for same-row deletions:
	 * each earlier cursor on the same row shifted this one left by original_len */
	for (i = 1; i < E.multi_count; i++) {
		int earlier = 0;
		int j;
		for (j = 0; j < i; j++) {
			if (E.multi_cursors[j].row == E.multi_cursors[i].row)
				earlier++;
		}
		E.multi_cursors[i].col -= earlier * E.multi_original_len;
	}

	/* Store base columns */
	for (i = 0; i < E.multi_count; i++)
		E.multi_cursors[i].base_col = E.multi_cursors[i].col;

	/* Clear selection, enter multi-cursor mode */
	editorClearSelection();
	E.multi_active = 1;
	E.cx = E.multi_cursors[0].col;
	E.cy = E.multi_cursors[0].row;

	editorSetStatusMessage("Multi-edit: %d cursors | Type replacement | Enter=done | Esc=cancel",
	                       E.multi_count);
}

/*
 * Confirm multi-cursor replace: push undo entry and exit.
 */
static void editorMultiCursorConfirm(void)
{
	/* Build undo entry: str = "original\0replacement" */
	UndoEntry ue = {0};
	ue.type = UNDO_MULTI_REPLACE;
	ue.old_cx = E.cx;
	ue.old_cy = E.cy;
	ue.c = E.multi_original_len;
	ue.indent = E.multi_typed_len;

	int total = E.multi_original_len + 1 + E.multi_typed_len;
	ue.str = malloc(total);
	memcpy(ue.str, E.multi_original, E.multi_original_len);
	ue.str[E.multi_original_len] = '\0';
	if (E.multi_typed_len > 0) {
		/* Extract replacement text from first cursor position */
		memcpy(ue.str + E.multi_original_len + 1,
		       &E.row[E.multi_cursors[0].row].chars[E.multi_cursors[0].col],
		       E.multi_typed_len);
	}
	ue.str_len = total;

	undoStackPush(&undo_stack, ue);
	undoStackClear(&redo_stack);

	editorSetStatusMessage("Replaced %d occurrences", E.multi_count);

	free(E.multi_original);
	E.multi_original = NULL;
	E.multi_active = 0;
	E.multi_count = 0;
	E.dirty++;
}

/*
 * Cancel multi-cursor replace: delete typed text, re-insert original word.
 */
static void editorMultiCursorCancel(void)
{
	int i;

	/* Remove typed text at each cursor (end to start) */
	if (E.multi_typed_len > 0) {
		for (i = E.multi_count - 1; i >= 0; i--) {
			editorRowDeleteChars(&E.row[E.multi_cursors[i].row],
			                     E.multi_cursors[i].col, E.multi_typed_len);
		}
	}

	/* Re-insert original word at each cursor (end to start) */
	for (i = E.multi_count - 1; i >= 0; i--) {
		editorRowInsertString(&E.row[E.multi_cursors[i].row],
		                      E.multi_cursors[i].base_col,
		                      E.multi_original, E.multi_original_len);
	}

	editorSetStatusMessage("Multi-edit cancelled");

	free(E.multi_original);
	E.multi_original = NULL;
	E.multi_active = 0;
	E.multi_count = 0;
}

/*
 * Handle keypress in multi-cursor mode.
 * Only printable chars, backspace, Enter (confirm), and Escape (cancel).
 */
void editorMultiCursorKey(int c)
{
	if (c == '\r') {
		editorMultiCursorConfirm();
		return;
	}

	if (c == '\x1b') {
		editorMultiCursorCancel();
		return;
	}

	if (c == BACKSPACE || c == CTRL_KEY('h')) {
		if (E.multi_typed_len == 0) return;

		/* Delete one char at each cursor position (end to start) */
		int i;
		for (i = E.multi_count - 1; i >= 0; i--) {
			int col = E.multi_cursors[i].col + E.multi_typed_len - 1;
			editorRowDeleteChar(&E.row[E.multi_cursors[i].row], col);
		}
		E.multi_typed_len--;
		editorMultiCursorRecalcCols();
		E.cx = E.multi_cursors[0].col + E.multi_typed_len;
		E.cy = E.multi_cursors[0].row;
		return;
	}

	/* Printable character: insert at ALL cursor positions (end to start) */
	if (c >= 32 && c < 127) {
		int i;
		for (i = E.multi_count - 1; i >= 0; i--) {
			int col = E.multi_cursors[i].col + E.multi_typed_len;
			editorRowInsertChar(&E.row[E.multi_cursors[i].row], col, c);
		}
		E.multi_typed_len++;
		editorMultiCursorRecalcCols();
		E.cx = E.multi_cursors[0].col + E.multi_typed_len;
		E.cy = E.multi_cursors[0].row;
		return;
	}

	/* Ignore all other keys in multi-cursor mode */
}

/* --- Undo/Redo --- */

/*
 * Undo the most recent edit operation.
 * Pops from undo_stack, reverses the operation using low-level primitives
 * (NOT the high-level functions, to avoid recording undo entries for undo itself),
 * then pushes a redo entry so the operation can be re-applied.
 */
void editorUndo(void)
{
	UndoEntry ue;
	if (!undoStackPop(&undo_stack, &ue)) {
		editorSetStatusMessage("Nothing to undo");
		return;
	}

	/* Build redo entry — same type and data, so redo can re-apply */
	UndoEntry re = {0};
	re.type = ue.type;
	re.old_cx = ue.old_cx;
	re.old_cy = ue.old_cy;
	re.c = ue.c;

	switch (ue.type) {
		case UNDO_INSERT_CHAR:
			/* Undo: remove the character that was inserted */
			editorRowDeleteChar(&E.row[ue.old_cy], ue.old_cx);
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_DELETE_CHAR:
			/* Undo: re-insert the character that was deleted */
			editorRowInsertChar(&E.row[ue.old_cy], ue.old_cx - 1, ue.c);
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_JOIN_LINES:
			/* Undo: split the joined line back into two */
			{
				/* The join appended ue.str to row[old_cy-1].
				 * To reverse: truncate row[old_cy-1] and insert row[old_cy] with ue.str. */
				int join_col = E.row[ue.old_cy - 1].size - ue.str_len;

				/* Insert the saved row text back */
				editorInsertRow(ue.old_cy, ue.str, ue.str_len);

				/* Truncate the previous row at the join point */
				E.row[ue.old_cy - 1].size = join_col;
				E.row[ue.old_cy - 1].chars[join_col] = '\0';
				editorUpdateSyntax(&E.row[ue.old_cy - 1], ue.old_cy - 1);

				/* Save the string for redo */
				re.str = malloc(ue.str_len);
				memcpy(re.str, ue.str, ue.str_len);
				re.str_len = ue.str_len;
			}
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_INSERT_NEWLINE:
			/* Undo: rejoin the two lines that were split */
			re.indent = ue.indent;  /* preserve indent count for redo */
			if (ue.old_cx == 0) {
				/* Was a blank line inserted above — just delete it */
				editorDeleteRow(ue.old_cy);
			} else {
				/*
				 * Strip auto-indent chars from the new row before rejoining.
				 * The indent chars were inserted AFTER the split, so we need
				 * to remove them to get back to the original text.
				 */
				erow *newrow = &E.row[ue.old_cy + 1];
				if (ue.indent > 0 && newrow->size >= ue.indent) {
					memmove(newrow->chars, &newrow->chars[ue.indent],
					        newrow->size - ue.indent + 1);
					newrow->size -= ue.indent;
				}

				/* Rejoin: append row[old_cy+1] to row[old_cy], then delete */
				editorRowAppendString(&E.row[ue.old_cy],
					E.row[ue.old_cy + 1].chars, E.row[ue.old_cy + 1].size);
				editorDeleteRow(ue.old_cy + 1);
			}
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_DUPLICATE_LINE:
			/* Undo: delete the duplicated row */
			editorDeleteRow(ue.old_cy + 1);
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_DELETE_LINE:
			/* Undo: re-insert the deleted line */
			editorInsertRow(ue.old_cy, ue.str, ue.str_len);
			re.str = malloc(ue.str_len);
			memcpy(re.str, ue.str, ue.str_len);
			re.str_len = ue.str_len;
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_INSERT_STRING:
			/* Undo: delete the grouped string */
			editorRowDeleteChars(&E.row[ue.old_cy], ue.old_cx, ue.str_len);
			re.str = malloc(ue.str_len);
			memcpy(re.str, ue.str, ue.str_len);
			re.str_len = ue.str_len;
			E.cx = ue.old_cx;
			E.cy = ue.old_cy;
			break;

		case UNDO_MULTI_REPLACE:
			/* Undo: find all replacement words, replace with original.
			 * str = "original\0replacement", c = orig_len, indent = rep_len */
			{
				char *orig = ue.str;
				int orig_len = ue.c;
				char *rep = ue.str + orig_len + 1;
				int rep_len = ue.indent;

				/* Save redo data */
				re.str = malloc(ue.str_len);
				memcpy(re.str, ue.str, ue.str_len);
				re.str_len = ue.str_len;
				re.c = ue.c;
				re.indent = ue.indent;

				/* Scan file for replacement, replace with original (end to start) */
				int r;
				for (r = E.numrows - 1; r >= 0; r--) {
					int pos = E.row[r].size - rep_len;
					while (pos >= 0) {
						if (strncmp(&E.row[r].chars[pos], rep, rep_len) == 0) {
							int left_ok = (pos == 0 || is_separator(E.row[r].chars[pos - 1]));
							int right_ok = (pos + rep_len >= E.row[r].size ||
							               is_separator(E.row[r].chars[pos + rep_len]));
							if (left_ok && right_ok) {
								editorRowDeleteChars(&E.row[r], pos, rep_len);
								editorRowInsertString(&E.row[r], pos, orig, orig_len);
							}
						}
						pos--;
					}
				}
				E.cx = ue.old_cx;
				E.cy = ue.old_cy;
			}
			break;
	}

	free(ue.str);
	undoStackPush(&redo_stack, re);
}

/*
 * Redo the most recently undone operation.
 * Pops from redo_stack, re-applies the operation, pushes undo entry back.
 */
void editorRedo(void)
{
	UndoEntry re;
	if (!undoStackPop(&redo_stack, &re)) {
		editorSetStatusMessage("Nothing to redo");
		return;
	}

	/* Build undo entry so this can be undone again */
	UndoEntry ue = {0};
	ue.type = re.type;
	ue.old_cx = re.old_cx;
	ue.old_cy = re.old_cy;
	ue.c = re.c;

	switch (re.type) {
		case UNDO_INSERT_CHAR:
			/* Redo: re-insert the character */
			if (re.old_cy == E.numrows)
				editorInsertRow(E.numrows, "", 0);
			editorRowInsertChar(&E.row[re.old_cy], re.old_cx, re.c);
			E.cx = re.old_cx + 1;
			E.cy = re.old_cy;
			break;

		case UNDO_DELETE_CHAR:
			/* Redo: re-delete the character */
			ue.c = E.row[re.old_cy].chars[re.old_cx - 1];
			editorRowDeleteChar(&E.row[re.old_cy], re.old_cx - 1);
			E.cx = re.old_cx - 1;
			E.cy = re.old_cy;
			break;

		case UNDO_JOIN_LINES:
			/* Redo: re-join the lines (repeat the original backspace at col 0) */
			{
				erow *row = &E.row[re.old_cy];
				ue.str = malloc(row->size);
				memcpy(ue.str, row->chars, row->size);
				ue.str_len = row->size;

				E.cx = E.row[re.old_cy - 1].size;
				editorRowAppendString(&E.row[re.old_cy - 1], row->chars, row->size);
				editorDeleteRow(re.old_cy);
				E.cy = re.old_cy - 1;
			}
			break;

		case UNDO_INSERT_NEWLINE:
			/* Redo: re-split the line */
			ue.indent = re.indent;  /* preserve indent count */
			if (re.old_cx == 0) {
				editorInsertRow(re.old_cy, "", 0);
			} else {
				erow *row = &E.row[re.old_cy];
				editorInsertRow(re.old_cy + 1, &row->chars[re.old_cx],
				               row->size - re.old_cx);
				row = &E.row[re.old_cy];  /* re-read after realloc */
				row->size = re.old_cx;
				row->chars[row->size] = '\0';
				editorUpdateSyntax(row, re.old_cy);
			}
			E.cy = re.old_cy + 1;
			/* Re-insert auto-indent chars */
			if (re.indent > 0 && re.old_cy < E.numrows) {
				erow *prev = &E.row[re.old_cy];
				int i;
				for (i = 0; i < re.indent && i < prev->size; i++)
					editorRowInsertChar(&E.row[E.cy], i, prev->chars[i]);
				E.cx = re.indent;
			} else {
				E.cx = 0;
			}
			break;

		case UNDO_DUPLICATE_LINE:
			/* Redo: re-duplicate the line */
			editorInsertRow(re.old_cy + 1,
				E.row[re.old_cy].chars, E.row[re.old_cy].size);
			E.cy = re.old_cy + 1;
			E.cx = re.old_cx;
			break;

		case UNDO_DELETE_LINE:
			/* Redo: re-delete the line */
			{
				erow *row = &E.row[re.old_cy];
				ue.str = malloc(row->size);
				memcpy(ue.str, row->chars, row->size);
				ue.str_len = row->size;
				editorDeleteRow(re.old_cy);
				if (E.cy >= E.numrows && E.numrows > 0)
					E.cy = E.numrows - 1;
				if (E.numrows > 0 && E.cx > E.row[E.cy].size)
					E.cx = E.row[E.cy].size;
			}
			break;

		case UNDO_INSERT_STRING:
			/* Redo: re-insert the grouped string */
			ue.str = malloc(re.str_len);
			memcpy(ue.str, re.str, re.str_len);
			ue.str_len = re.str_len;
			if (re.old_cy == E.numrows)
				editorInsertRow(E.numrows, "", 0);
			editorRowInsertString(&E.row[re.old_cy], re.old_cx, re.str, re.str_len);
			E.cx = re.old_cx + re.str_len;
			E.cy = re.old_cy;
			break;

		case UNDO_MULTI_REPLACE:
			/* Redo: find all original words, replace with replacement */
			{
				char *orig = re.str;
				int orig_len = re.c;
				char *rep = re.str + orig_len + 1;
				int rep_len = re.indent;

				/* Save undo data */
				ue.str = malloc(re.str_len);
				memcpy(ue.str, re.str, re.str_len);
				ue.str_len = re.str_len;
				ue.c = re.c;
				ue.indent = re.indent;

				/* Scan file for original, replace with replacement (end to start) */
				int r;
				for (r = E.numrows - 1; r >= 0; r--) {
					int pos = E.row[r].size - orig_len;
					while (pos >= 0) {
						if (strncmp(&E.row[r].chars[pos], orig, orig_len) == 0) {
							int left_ok = (pos == 0 || is_separator(E.row[r].chars[pos - 1]));
							int right_ok = (pos + orig_len >= E.row[r].size ||
							               is_separator(E.row[r].chars[pos + orig_len]));
							if (left_ok && right_ok) {
								editorRowDeleteChars(&E.row[r], pos, orig_len);
								editorRowInsertString(&E.row[r], pos, rep, rep_len);
							}
						}
						pos--;
					}
				}
				E.cx = re.old_cx;
				E.cy = re.old_cy;
			}
			break;
	}

	free(re.str);
	undoStackPush(&undo_stack, ue);
}
