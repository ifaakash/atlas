#include "atlas.h"

struct EditorConfig E;

/*
 * Initialize editor state — call once at startup.
 */
void initEditor(void)
{
	E.cx = 0;
	E.cy = 0;
	E.rowoff = 0;
	E.coloff = 0;
	E.numrows = 0;
	E.row = NULL;
	E.filename = NULL;
	E.dirty = 0;
	E.statusmsg[0] = '\0';
	E.statusmsg_time = 0;
	E.syntax = NULL;
	E.mouse_x = 0;
	E.mouse_y = 0;
	E.sel_active = 0;
	E.sel_anchor_x = 0;
	E.sel_anchor_y = 0;
	E.clipboard = NULL;
	E.clipboard_len = 0;
	E.line_number_width = 4;  /* default: 3 digits + 1 space */
	undoStackInit(&undo_stack);
	undoStackInit(&redo_stack);
	getWindowSize(&E.screenrows, &E.screencols);
	E.screenrows -= 2;  /* reserve 2 bottom rows for status bar + message bar */
}

/*
 * Central key handling — maps every keypress to an action.
 * This is the "controller" in the editor's main loop.
 */
void editorProcessKeypress(void)
{
	/*
	 * quit_times: safety counter for quitting with unsaved changes.
	 * Must press Ctrl+Q twice to force-quit a dirty buffer.
	 * Resets to 1 whenever any other key is pressed.
	 */
	static int quit_times = 1;

	int c = editorReadKey();

	switch (c) {
		case '\r':                          /* Enter key */
			editorInsertNewline();
			break;

		case CTRL_KEY('q'):                 /* Quit */
			if (E.dirty && quit_times > 0) {
				editorSetStatusMessage(
					"WARNING! Unsaved changes. Press Ctrl-Q again to quit.");
				quit_times--;
				return;  /* return early — don't reset quit_times below */
			}
			exit(0);  /* disableRawMode via atexit restores main screen buffer */
			break;

		case CTRL_KEY('s'):                 /* Save */
			editorSave();
			break;

		case CTRL_KEY('z'):                 /* Undo */
			editorUndo();
			break;

		case CTRL_KEY('y'):                 /* Redo */
			editorRedo();
			break;

		case CTRL_KEY('f'):                 /* Find/Search */
			editorFind();
			break;

		case CTRL_KEY('c'):                 /* Copy selection */
			if (E.sel_active) {
				editorCopySelection();
				editorClearSelection();
				editorSetStatusMessage("Copied to clipboard");
			}
			break;

		case CTRL_KEY('v'):                 /* Paste clipboard */
			editorClearSelection();
			editorPasteClipboard();
			break;

		case CTRL_KEY('d'):                 /* Duplicate line */
			editorClearSelection();
			editorDuplicateLine();
			break;

		case MOUSE_CLICK:                   /* Mouse click — position cursor */
			editorClearSelection();
			E.cy = E.mouse_y + E.rowoff;
			E.cx = (E.mouse_x - E.line_number_width) + E.coloff;
			if (E.cx < 0) E.cx = 0;  /* clicked in gutter */
			/* Clamp to file bounds */
			if (E.cy >= E.numrows)
				E.cy = E.numrows ? E.numrows - 1 : 0;
			if (E.cy < E.numrows && E.cx > E.row[E.cy].size)
				E.cx = E.row[E.cy].size;
			break;

		case SHIFT_UP:                      /* Shift+Arrow — extend selection */
		case SHIFT_DOWN:
		case SHIFT_LEFT:
		case SHIFT_RIGHT:
			editorStartSelection();
			/* Map shift keys to regular arrow movement */
			switch (c) {
				case SHIFT_UP:    editorMoveCursor(ARROW_UP); break;
				case SHIFT_DOWN:  editorMoveCursor(ARROW_DOWN); break;
				case SHIFT_LEFT:  editorMoveCursor(ARROW_LEFT); break;
				case SHIFT_RIGHT: editorMoveCursor(ARROW_RIGHT); break;
			}
			break;

		case ALT_LEFT:                      /* Option+Left — jump word left */
			editorClearSelection();
			editorMoveWordLeft();
			break;

		case ALT_RIGHT:                     /* Option+Right — jump word right */
			editorClearSelection();
			editorMoveWordRight();
			break;

		case SHIFT_ALT_LEFT:                /* Shift+Option+Left — select word left */
			editorStartSelection();
			editorMoveWordLeft();
			break;

		case SHIFT_ALT_RIGHT:               /* Shift+Option+Right — select word right */
			editorStartSelection();
			editorMoveWordRight();
			break;

		case ALT_BACKSPACE:                 /* Option+Backspace — delete word */
		case CTRL_KEY('w'):                 /* Ctrl+W — also delete word (universal) */
			editorClearSelection();
			editorDeleteWord();
			break;

		case MOUSE_SCROLL_UP:              /* Mouse wheel up — scroll 3 lines */
			{
				int i;
				for (i = 0; i < 3; i++) editorMoveCursor(ARROW_UP);
			}
			break;

		case MOUSE_SCROLL_DOWN:            /* Mouse wheel down — scroll 3 lines */
			{
				int i;
				for (i = 0; i < 3; i++) editorMoveCursor(ARROW_DOWN);
			}
			break;

		case CTRL_KEY('g'):                /* Go to line number */
			{
				char *input = editorPrompt("Go to line: %s", NULL);
				if (input) {
					int line = atoi(input);
					free(input);
					if (line > 0 && line <= E.numrows) {
						E.cy = line - 1;  /* user types 1-based, we're 0-based */
						E.cx = 0;
						E.rowoff = E.cy;  /* scroll so target line is at top */
						editorSetStatusMessage("Jumped to line %d", line);
					} else {
						editorSetStatusMessage("Invalid line number");
					}
				}
			}
			break;

		case BACKSPACE:                     /* Backspace key (127) */
		case CTRL_KEY('h'):                 /* Ctrl+H — historical backspace */
		case DEL_KEY:                       /* Delete key */
			if (c == DEL_KEY)
				editorMoveCursor(ARROW_RIGHT);  /* delete = backspace one position right */
			editorDeleteChar();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;

		case END_KEY:
			if (E.cy < E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				/* Move cursor a full page up or down */
				int times = E.screenrows;
				while (times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorClearSelection();
			editorMoveCursor(c);
			break;

		case '\t':                          /* Tab — auto-complete word */
			editorAutoComplete();
			break;

		case CTRL_KEY('l'):                 /* Refresh — no-op, redraws anyway */
		case '\x1b':                        /* Escape — ignore */
			break;

		default:
			editorInsertChar(c);            /* Normal character — insert it */
			break;
	}

	/* Any key other than Ctrl+Q resets the quit confirmation counter */
	quit_times = 1;
}

int main(int argc, char *argv[])
{
	enableRawMode();
	initEditor();

	/* If a filename was passed on the command line, open it */
	if (argc >= 2) {
		editorOpen(argv[1]);
	}

	editorSetStatusMessage("Ctrl-S=save | Ctrl-Q=quit | Ctrl-F=find | Ctrl-Z=undo | Ctrl-Y=redo");

	while (1) {
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}
