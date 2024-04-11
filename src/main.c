#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>

#include "modules.h"

bool string_contains(char *list, char *substr, char *ifs) {
	// checks if a substring is contained in list which is separated by ifs
	// the easiest way to do this is to put the ifs before and after the whole list, and the same with the substring
	// then check if it contains a substring

	size_t list_len = strlen(list);
	size_t substr_len = strlen(substr);
	size_t ifs_len = strlen(ifs);

	char *padded_list, *padded_substr;

	size_t padded_list_len = list_len + ifs_len * 2 + 1;
	if (!(padded_list = malloc(padded_list_len))) {
		warn("malloc");
		return false;
	}
	if (snprintf(padded_list, padded_list_len, "%s%s%s", ifs, list, ifs) < 0) {
		warnx("snprintf");
		return false;
	}

	size_t padded_substr_len = substr_len + ifs_len * 2 + 1;
	if (!(padded_substr = malloc(padded_substr_len))) {
		warn("malloc");
		return false;
	}
	if (snprintf(padded_substr, padded_substr_len, "%s%s%s", ifs, substr, ifs) < 0) {
		warnx("snprintf");
		return false;
	}

	bool result = strstr(padded_list, padded_substr);

	free(padded_list);
	free(padded_substr);

	return result;
}

void print_output(module_output output, bool allow_color, FILE *fp) {
	if (!output) return;
	if (!output[0].string) return;

	void print_newline() {
		fputc('\n', fp);
	}

	for (size_t i = 0; output[i].string; ++i) {
		struct colored_text text = output[i];
		char *str = text.string;
		if (!str || !*str) break;

		if (allow_color) {
			if (HAS_FLAG(text.flags, FLAG_BOLD)) fputs("\x1b[1m", fp);
			if (HAS_FLAG(text.flags, FLAG_ITALIC)) fputs("\x1b[3m", fp);
			if (HAS_FLAG(text.flags, FLAG_UNDERLINE)) fputs("\x1b[4m", fp);
			if (HAS_FLAG(text.flags, FLAG_STRIKETHROUGH)) fputs("\x1b[9m", fp);
			if (HAS_FLAG(text.flags, FLAG_FG_COLOR)) {
				fprintf(fp, "\x1b[38;5;%im", text.fg_color);
			}
			if (HAS_FLAG(text.flags, FLAG_BG_COLOR)) {
				fprintf(fp, "\x1b[48;5;%im", text.bg_color);
			}
		}

		// loop the new lines if we ever want to do something with them
		char *newline;
		while (str && *str) {
			newline = strchr(str, '\n');
			size_t len = newline - str;
			if (!newline) len = strlen(str); // newline is NULL on last line, so print the rest of the string
			fwrite(str, 1, len, fp);         // print the line
			if (newline) {
				print_newline();
				str = newline + 1;
			} else
				break;
		}
		if (allow_color)
			fprintf(fp, "\x1b[0m");
	}

	print_newline();
}

int main(int argc, char *argv[]) {
	// allow user to change field separator
	char *ifs = getenv("FO_IFS");
	if (!ifs) ifs = " ";

	char *modules_list = getenv("FO_MODULES");

	// list modules
	for (module *m = modules; m->name; ++m) {
		if (modules_list) {
			// only display if modules list contains the module
			if (!string_contains(modules_list, m->name, ifs)) continue;
		} else {
			//only display if it is set to display by default
			if (!m->display_by_default) continue;
		}
		module_output output = m->func(m);
		print_output(output, true, stdout);
	}
	return 0;
}
