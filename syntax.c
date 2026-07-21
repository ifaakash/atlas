#include "atlas.h"

/* --- Filetype database --- */

char *C_HL_extensions[] = { ".c", ".h", ".cpp", ".hpp", NULL };

char *C_HL_keywords[] = {
	"switch", "if", "while", "for", "break", "continue", "return",
	"else", "struct", "union", "typedef", "static", "enum", "case",
	"sizeof", "#include", "#define", "do",
	/* Type keywords end with '|' — rendered in a different color */
	"int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
	"void|", "size_t|", "ssize_t|", "FILE|", NULL
};

/* HLDB: the highlight database. Add new languages here. */
struct editorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//", "/*", "*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

unsigned int HLDB_ENTRIES = sizeof(HLDB) / sizeof(HLDB[0]);

/* --- Syntax highlighting --- */

/* Returns 1 if 'c' is a separator character (space, punctuation, or null) */
int is_separator(int c)
{
	return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];:!&|{}", c) != NULL;
}

/* Maps a highlight type to an ANSI color code (foreground colors 30-37) */
/*
 * Maps highlight type to a 256-color code.
 * Used with \x1b[38;5;NNm (foreground) in the rendering loop.
 */
int editorSyntaxToColor(int hl)
{
	switch (hl) {
		case HL_NUMBER:     return 208;  /* orange */
		case HL_MATCH:      return 226;  /* bright yellow (search match) */
		case HL_STRING:     return 114;  /* green */
		case HL_COMMENT:
		case HL_MLCOMMENT:  return 243;  /* gray */
		case HL_KEYWORD1:   return 170;  /* purple */
		case HL_KEYWORD2:   return 75;   /* blue */
		default:            return 252;  /* light gray */
	}
}

/*
 * Updates the hl[] array for a single row — assigns a highlight type
 * to every character based on syntax rules.
 *
 * This is the most complex function in the editor. It walks through
 * each character and decides if it's part of a number, string, comment,
 * keyword, or just normal text.
 *
 * 'row_index' is needed to check the previous row's open comment state
 * and to cascade updates to the next row if our state changed.
 */
void editorUpdateSyntax(erow *row, int row_index)
{
	row->hl = realloc(row->hl, row->size);
	memset(row->hl, HL_NORMAL, row->size);

	if (E.syntax == NULL) return;

	char **keywords = E.syntax->keywords;
	char *scs = E.syntax->singleline_comment_start;
	char *mcs = E.syntax->multiline_comment_start;
	char *mce = E.syntax->multiline_comment_end;

	int scs_len = scs ? strlen(scs) : 0;
	int mcs_len = mcs ? strlen(mcs) : 0;
	int mce_len = mce ? strlen(mce) : 0;

	int prev_sep = 1;         /* previous char was a separator (start of line = yes) */
	int in_string = 0;        /* 0 = not in string, or the quote char if inside */
	int in_comment = (row_index > 0 && E.row[row_index - 1].hl_open_comment);

	int i = 0;
	while (i < row->size) {
		char c = row->chars[i];
		unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

		/* --- Single-line comments --- */
		if (scs_len && !in_string && !in_comment) {
			if (!strncmp(&row->chars[i], scs, scs_len)) {
				/* Rest of line is a comment */
				memset(&row->hl[i], HL_COMMENT, row->size - i);
				break;
			}
		}

		/* --- Multi-line comments --- */
		if (mcs_len && mce_len && !in_string) {
			if (in_comment) {
				row->hl[i] = HL_MLCOMMENT;
				if (!strncmp(&row->chars[i], mce, mce_len)) {
					memset(&row->hl[i], HL_MLCOMMENT, mce_len);
					i += mce_len;
					in_comment = 0;
					prev_sep = 1;
					continue;
				} else {
					i++;
					continue;
				}
			} else if (!strncmp(&row->chars[i], mcs, mcs_len)) {
				memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
				i += mcs_len;
				in_comment = 1;
				continue;
			}
		}

		/* --- Strings --- */
		if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
			if (in_string) {
				row->hl[i] = HL_STRING;
				/* Handle backslash escapes (e.g., \" inside a string) */
				if (c == '\\' && i + 1 < row->size) {
					row->hl[i + 1] = HL_STRING;
					i += 2;
					continue;
				}
				if (c == in_string) in_string = 0;  /* closing quote */
				i++;
				prev_sep = 1;
				continue;
			} else {
				if (c == '"' || c == '\'') {
					in_string = c;
					row->hl[i] = HL_STRING;
					i++;
					continue;
				}
			}
		}

		/* --- Numbers --- */
		if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
			if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
			    (c == '.' && prev_hl == HL_NUMBER)) {
				row->hl[i] = HL_NUMBER;
				i++;
				prev_sep = 0;
				continue;
			}
		}

		/* --- Keywords --- */
		if (prev_sep) {
			int j;
			for (j = 0; keywords[j]; j++) {
				int klen = strlen(keywords[j]);
				/* Keywords ending with '|' are type keywords (HL_KEYWORD2) */
				int kw2 = keywords[j][klen - 1] == '|';
				if (kw2) klen--;

				/* Match keyword and ensure it's followed by a separator */
				if (!strncmp(&row->chars[i], keywords[j], klen) &&
				    is_separator(row->chars[i + klen])) {
					memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
					i += klen;
					break;
				}
			}
			if (keywords[j] != NULL) {
				prev_sep = 0;
				continue;
			}
		}

		prev_sep = is_separator(c);
		i++;
	}

	/* Track whether this row ends inside a multi-line comment */
	int changed = (row->hl_open_comment != in_comment);
	row->hl_open_comment = in_comment;

	/*
	 * If our open-comment state changed, the NEXT row's highlighting
	 * might be wrong — cascade the update. This handles multi-line
	 * comments that span many rows.
	 */
	if (changed && row_index + 1 < E.numrows)
		editorUpdateSyntax(&E.row[row_index + 1], row_index + 1);
}

/*
 * Detects the filetype from E.filename and sets E.syntax.
 * Checks each HLDB entry's filematch patterns against the filename extension.
 */
void editorSelectSyntaxHighlight(void)
{
	E.syntax = NULL;
	if (E.filename == NULL) return;

	/* Find the last '.' in the filename — that's the extension */
	char *ext = strrchr(E.filename, '.');

	unsigned int j;
	for (j = 0; j < HLDB_ENTRIES; j++) {
		struct editorSyntax *s = &HLDB[j];
		unsigned int i = 0;

		while (s->filematch[i]) {
			int is_ext = (s->filematch[i][0] == '.');

			if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
			    (!is_ext && strstr(E.filename, s->filematch[i]))) {
				E.syntax = s;

				/* Re-highlight all rows with the new syntax */
				int filerow;
				for (filerow = 0; filerow < E.numrows; filerow++) {
					editorUpdateSyntax(&E.row[filerow], filerow);
				}
				return;
			}
			i++;
		}
	}
}
