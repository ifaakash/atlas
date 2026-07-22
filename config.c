#include "atlas.h"

/*
 * Load configuration from ~/.atlasrc.
 * Simple key=value format, one setting per line.
 * Lines starting with '#' are comments. Unknown keys are ignored.
 */
void editorLoadConfig(void)
{
	char *home = getenv("HOME");
	if (!home) return;

	char path[256];
	snprintf(path, sizeof(path), "%s/.atlasrc", home);

	FILE *fp = fopen(path, "r");
	if (!fp) return;  /* no config file — use defaults */

	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		/* Skip empty lines and comments */
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;

		/* Find '=' separator */
		char *eq = strchr(line, '=');
		if (!eq) continue;

		/* Extract key (trim trailing whitespace) */
		char *key_start = line;
		while (*key_start == ' ' || *key_start == '\t') key_start++;
		char *key_end = eq - 1;
		while (key_end > key_start && (*key_end == ' ' || *key_end == '\t'))
			key_end--;
		int key_len = key_end - key_start + 1;
		if (key_len <= 0) continue;

		/* Extract value (trim whitespace and newline) */
		char *val_start = eq + 1;
		while (*val_start == ' ' || *val_start == '\t') val_start++;
		char *val_end = val_start + strlen(val_start) - 1;
		while (val_end > val_start &&
		       (*val_end == '\n' || *val_end == '\r' ||
		        *val_end == ' ' || *val_end == '\t'))
			val_end--;
		*(val_end + 1) = '\0';

		int val = atoi(val_start);

		/* Match known settings */
		if (key_len == 9 && strncmp(key_start, "tab_width", 9) == 0) {
			if (val >= 1 && val <= 16) E.tab_width = val;
		} else if (key_len == 17 && strncmp(key_start, "show_line_numbers", 17) == 0) {
			E.show_line_numbers = (val != 0);
		} else if (key_len == 9 && strncmp(key_start, "syntax_on", 9) == 0) {
			E.syntax_on = (val != 0);
		} else if (key_len == 11 && strncmp(key_start, "auto_indent", 11) == 0) {
			E.auto_indent = (val != 0);
		}
	}

	fclose(fp);
}
