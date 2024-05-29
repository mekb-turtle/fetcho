#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <pwd.h>
#include <err.h>
#include <proc/sysinfo.h>

#include "modules.h"

/*
static void *pending_free[16];
static size_t pending_free_i = 0;
void free_later(void *ptr) {
	if (pending_free_i >= (sizeof(pending_free) / sizeof(pending_free[0]))) return;
	pending_free[pending_free_i++]=ptr;
}
*/

static struct utsname *get_utsname() {
	static struct utsname *un;
	static bool init = false;
	if (!init) {
		init = true;
		static struct utsname un_;
		if (uname(&un_) < 0)
			un = NULL;
		else
			un = &un_;
	}
	return un;
}

static struct passwd *get_passwd() {
	static struct passwd *passwd;
	static bool init = false;
	if (!init) {
		init = true;
		passwd = getpwuid(getuid()); // getuid always succeeds
	}
	return passwd;
}

static struct sysinfo *get_sysinfo() {
	static struct sysinfo *si = NULL;
	static bool init = false;
	if (!init) {
		init = true;
		static struct sysinfo si_;
		if (sysinfo(&si_) < 0)
			si = NULL;
		else
			si = &si_;
	}
	return si;
}

static char *get_basename(char *path) {
	char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

enum format_bytes_mode { binary_i,
	                     binary,
	                     metric };

static char *format_bytes(size_t byte, enum format_bytes_mode mode) {
	const int scale = 2;

	size_t scale_exp = 1;
	for (int i = 0; i < scale; ++i) scale_exp *= 10;

	char *suffix = "";
	int base = 0;
	switch (mode) {
		case metric:
			base = 1000;
			break;
		case binary_i:
			suffix = "i";
		case binary:
			base = 1024;
			break;
		default:
			break;
	}

	size_t power = 1;
	int exp_index = 0;
	size_t mantissa = byte;
	while (mantissa >= base) {
		++exp_index;      // increase exponent
		mantissa /= base; // divide by base
		power *= base;    // multiply power by the base so we can count the remainder
	}

	size_t remainder = byte - mantissa * power;
	float fraction_part = (remainder * scale_exp) / (float) power / (float) scale_exp;
	if (remainder == 0) fraction_part = 0; // prevent floating point weirdness

	char *frac_str, *str;
	if (!(frac_str = malloc(64))) {
		warn("malloc");
		return NULL;
	}

	// fraction part in a separate string so we can trim off until the decimal point
	if (snprintf(frac_str, 64, "%.*g", scale, fraction_part) < 0) {
		warnx("snprintf");
		free(frac_str);
		return NULL;
	}

	if (!(str = malloc(64))) {
		warn("malloc");
		free(frac_str);
		return NULL;
	}

	char *frac_str_dp = strchrnul(frac_str, '.'); // find the decimal point, or "" if none found
	if (strchr(frac_str, 'e')) {
		// use "" if float uses scientific notation
		frac_str_dp = frac_str + strlen(frac_str);
	}
	if (strlen(frac_str_dp) >= scale + 1) {
		// trim string if it's too long
		frac_str_dp[scale + 1] = '\0';
	}

	const char *suffixes = " kMGTPEZY";

	if (snprintf(str, 64, "%zu%s %c%s", mantissa, frac_str_dp, suffixes[exp_index], suffix) < 0)
		goto snprintf_error;

	if (byte == 0) {
		if (snprintf(str, 64, "0") < 0) // don't bother printing "bytes" for 0
			goto snprintf_error;
	} else if (exp_index == 0) { // append "bytes" instead
		char *trim = strchr(str, ' ');
		if (trim) {
			if (snprintf(trim, 64 - (trim - str), " byte%s", byte == 1 ? "" : "s") < 0)
				goto snprintf_error;
		}
	}

	free(frac_str);

	return str;

snprintf_error:
	warnx("snprintf");
	free(frac_str);
	free(str);
	return NULL;
}

static char *format_time(unsigned long total) {
	unsigned long s = total;

	const unsigned long second = 1;
	const unsigned long minute = second * 60;
	const unsigned long hour = minute * 60;
	const unsigned long day = hour * 24;
	const unsigned long week = day * 7;

	unsigned long weeks = s / week;
	s %= week;
	uint8_t days = s / day;
	s %= day;
	uint8_t hours = s / hour;
	s %= hour;
	uint8_t minutes = s / minute;
	s %= minute;
	uint8_t seconds = s / second;
	s %= second;

	char *str;
	if (!(str = malloc(128))) {
		warn("malloc");
		return NULL;
	}
	int index = 0, result = 0;

	if (total >= week) {
		if ((result = snprintf(str + index, 128, "%luw ", weeks)) < 0) goto snprintf_error;
		index += result;
	}
	if (total >= day) {
		if ((result = snprintf(str + index, 128, "%id ", days)) < 0) goto snprintf_error;
		index += result;
	}
	if (total >= hour) {
		if ((result = snprintf(str + index, 128, "%ih ", hours)) < 0) goto snprintf_error;
		index += result;
	}
	if (total >= minute) {
		if ((result = snprintf(str + index, 128, "%im ", minutes)) < 0) goto snprintf_error;
		index += result;
	}
	if (total >= second) {
		if ((result = snprintf(str + index, 128, "%is ", seconds)) < 0) goto snprintf_error;
		index += result;
	}

	str[index - 1] = '\0'; // trim leading space and comma

	return str;

snprintf_error:
	warnx("snprintf");
	free(str);
	return NULL;
}

static char *get_hostname(void) {
	struct utsname *un = get_utsname();
	if (!un) return NULL;
	return un->nodename;
}

static char *get_username(void) {
	struct passwd *passwd = get_passwd();
	if (!passwd) return NULL;
	return passwd->pw_name;
}

static bool read_file(FILE *fp, void **data_, size_t *size_, const char *filename) {
	// read a file in chunks
	const size_t read_size = 0x2000;

	void *data = malloc(read_size);
	if (!data) {
		warn("malloc");
		return false;
	}

	size_t size = 0;
	size_t read;
	while ((read = fread(data + size, 1, read_size, fp)) > 0) {
		size += read;
		// allocate the block to a bigger size
		void *new_data = realloc(data, size + read_size);
		if (!new_data) {
			free(data);
			warn("realloc");
			return false;
		}
		data = new_data;
	}

	if (ferror(fp) || !feof(fp)) {
		free(data);
		warnx("error reading file%s%s", filename ? ": " : "", filename ? filename : "");
		return false;
	}

	*data_ = data;
	*size_ = size;
	return true;
}

static bool read_filename(const char *filename, void **data, size_t *size) {
	FILE *fp = fopen(filename, "rb");
	if (!fp) return false;

	// save result and return it
	bool ret = read_file(fp, data, size, filename);
	fclose(fp);
	return ret;
}

static size_t get_first_line(void *data, size_t size) {
	void *eof = memchr(data, '\n', size);
	if (eof) return eof - data;
	return size;
}

char *parse_key_value_pair_list(char *key, char *data, size_t size) {
	// note: this function does not assume data ends with '\0'
	// use mem* functions for data, not str*

	size_t key_len = strlen(key);

	char *endptr = data + size;

	for (char *next_line = NULL; data && data < endptr; data = next_line) {
		size_t len = endptr - data;

		char *equal = memchr(data, '=', len); // pointer to next equals symbol
		char *eol = memchr(data, '\n', len);  // pointer to end of line
		if (!equal) break;
		next_line = eol ? eol + 1 : NULL;  // pointer to the next line, or NULL if this is the last line
		if (eol && equal >= eol) continue; // if the next equals sign is not on this line, skip to the next line
		if (data[0] == '#') continue;      // skip commented lines

		if (equal - data != key_len) continue;         // skip this line if the length of the keys do not match
		if (memcmp(data, key, key_len) != 0) continue; // skip this line if the name of the keys do not match

		// found the matching line
		char *value = equal + 1;
		size_t value_len = eol - value;

		// trim surrounding quotes
		if (
		        (value[0] == '\'' && value[value_len - 1] == '\'') ||
		        (value[0] == '"' && value[value_len - 1] == '"')) {
			++value;
			value_len -= 2;
		}

		char *output = malloc(value_len + 1);
		if (!output) {
			warn("malloc");
			break;
		}

		memcpy(output, value, value_len);
		output[value_len] = '\0';

		return output;
	}

	return NULL;
}

bool getenv_bool(const char *name) {
	char *nerd = getenv(name);
	if (!nerd) return false;
	if (*nerd == '\0') return false;
	else if (strcasecmp(nerd, "n") == 0)
		return false;
	else if (strcasecmp(nerd, "false") == 0)
		return false;
	else if (strcasecmp(nerd, "0") == 0)
		return false;
	else if (strcasecmp(nerd, "no") == 0)
		return false;
	return true;
}

static module_output line(char *string, bool free_string, module *module) {
	if (!string) return NULL;
	if (!module) return NULL;

	static bool init_nerd = false;
	static bool use_nerd;
	if (!init_nerd) {
		use_nerd = getenv_bool("FO_NERDFONTS");
		char *term = getenv("TERM");
		if (term && strcasecmp(term, "linux") == 0) use_nerd = false; // disable in tty
	}

	char *name = use_nerd ? module->symbol : module->name;
	const size_t padded_len = use_nerd ? 4 : 9;

	static int color = 0;
	// rainbow gradient
	int c = (int[]){1, 3, 2, 6, 4, 5}[color];
	color = (color + 1) % 6;

	module_output out = calloc(4, sizeof(struct colored_text));
	if (!out) {
		warn("calloc");
		return NULL;
	}

	size_t index = 0;
	out[index++] = (struct colored_text){.string = name, .free = false, .fg_color = c, .flags = FLAG_FG_COLOR | FLAG_BOLD};

	size_t name_len = use_nerd ? 2 : strlen(module->name);
	if (name_len < padded_len) {
		// create spacing to pad module name to 9 chars
		size_t spacing_size = padded_len - name_len;
		char *spacing = malloc(spacing_size + 1);
		if (!spacing) {
			warn("malloc");
			free(out);
			return NULL;
		}
		memset(spacing, ' ', spacing_size);
		spacing[spacing_size] = '\0';
		out[index++] = (struct colored_text){.string = spacing, .free = true, .flags = 0};
	}

	out[index++] = (struct colored_text){.string = string, .free = free_string, .flags = 0};
	out[index++] = (struct colored_text){.string = NULL};
	return out;
}

module_output module_hostname(module *mod) {
	return line(get_hostname(), false, mod);
}

module_output module_username(module *mod) {
	return line(get_username(), false, mod);
}

module_output module_header(module *mod) {
	char *user = get_username();
	char *host = get_hostname();

	module_output out = calloc(4, sizeof(struct colored_text));
	if (!out) {
		warn("calloc");
		return NULL;
	}

	memcpy(
	        out,
	        (struct colored_text[]){
	                {.string = user, .free = false, .flags = FLAG_FG_COLOR | FLAG_BOLD, .fg_color = 5},
	                {.string = "@", .free = false, .flags = FLAG_FG_COLOR | FLAG_BOLD, .fg_color = 2},
	                {.string = host, .free = false, .flags = FLAG_FG_COLOR | FLAG_BOLD, .fg_color = 5},
	                {.string = NULL}
    },
	        4 * sizeof(struct colored_text));

	return out;
}

module_output module_line(module *mod) {
	size_t line_len = strlen(get_username()) + strlen(get_hostname()) + 1;

	char *repeat_char = getenv("FO_LINETEXT");
	if (!repeat_char) repeat_char = "─";
	size_t repeat_char_len = strlen(repeat_char);

	size_t str_len = repeat_char_len * line_len;
	char *str = malloc(str_len + 1);
	if (!str) {
		warn("malloc");
		return NULL;
	}
	for (size_t i = 0; i < line_len; ++i) {
		memcpy(&str[repeat_char_len * i], repeat_char, repeat_char_len);
	}
	str[str_len] = '\0';

	module_output out = calloc(2, sizeof(struct colored_text));
	if (!out) {
		warn("calloc");
		free(str);
		return NULL;
	}

	memcpy(
	        out,
	        (struct colored_text[]){
	                {.string = str, .free = true, .flags = FLAG_BOLD},
	                {.string = NULL}
    },
	        2 * sizeof(struct colored_text));

	return out;
}

module_output module_os(module *mod) {
	const char *os_release_file = "/etc/os-release";

	void *data;
	size_t size;
	if (!read_filename(os_release_file, &data, &size)) return NULL;

	char *name = NULL;
	if (!name) name = parse_key_value_pair_list("PRETTY_NAME", data, size);
	if (!name) name = parse_key_value_pair_list("NAME", data, size);
	if (!name) name = parse_key_value_pair_list("ID", data, size);

	free(data);

	return line(name, true, mod);
}

module_output module_kernel(module *mod) {
	struct utsname *un = get_utsname();
	if (!un) return NULL;

	size_t sysname_len = strlen(un->sysname);
	size_t release_len = strlen(un->release);
	size_t total_len = sysname_len + release_len + 2;
	char *out = malloc(total_len);
	if (!out) {
		warn("malloc");
		return NULL;
	}
	if (snprintf(out, total_len, "%s %s", un->sysname, un->release) < 0) {
		warnx("snprintf");
		free(out);
		return NULL;
	}

	return line(out, true, mod);
}

module_output module_uptime(module *mod) {
	struct sysinfo *si = get_sysinfo();
	if (!si) return NULL;

	return line(format_time(si->uptime), true, mod);
}

module_output module_shell(module *mod) {
	struct passwd *passwd = get_passwd();
	if (!passwd) return NULL;

	return line(get_basename(passwd->pw_shell), false, mod);
}

const enum format_bytes_mode bytes_mode = binary_i;

module_output module_byte_display(size_t used, size_t total, module *mod) {
	char *used_str = format_bytes(used, bytes_mode);
	char *total_str = format_bytes(total, bytes_mode);

	size_t str_len = strlen(used_str) + strlen(total_str) + 4;
	char *str = malloc(str_len);
	if (!str) {
		warn("malloc");
		free(used_str);
		free(total_str);
		return NULL;
	}
	if (snprintf(str, str_len, "%s / %s", used_str, total_str) < 0) {
		warnx("snprintf");
		free(used_str);
		free(total_str);
		free(str);
		return NULL;
	}

	free(used_str);
	free(total_str);

	return line(str, true, mod);
}

module_output module_ram(module *mod) {
	meminfo(); // procps
	return module_byte_display(kb_main_used * 1024, kb_main_total * 1024, mod);
}

module_output module_swap(module *mod) {
	meminfo(); // procps
	return module_byte_display(kb_swap_used * 1024, kb_swap_total * 1024, mod);
}

module_output module_de(module *mod) {
	char *de = getenv("XDG_CURRENT_DESKTOP");
	if (!de) return NULL;

	char *result = NULL;

	if (strcasestr(de, "kde") || strcasestr(de, "Plasma"))
		result = "KDE Plasma";
	else if (strcasestr(de, "cinnamon"))
		result = "Cinnamon";
	else if (strcasestr(de, "lxqt"))
		result = "LXQt";
	else if (strcasestr(de, "lxde"))
		result = "LXDE";
	else if (strcasestr(de, "deepin"))
		result = "Deepin";
	else if (strcasestr(de, "enlightenment"))
		result = "Enlightenment";
	else if (strcasestr(de, "budgie"))
		result = "Budgie";
	else if (strcasestr(de, "Pantheon"))
		result = "Pantheon";
	else if (getenv("TDE_FULL_SESSION"))
		result = "Trinity";
	else if (getenv("MATE_DESKTOP_SESSION_ID") || strcasestr(de, "mate"))
		result = "MATE";
	else if (strcasestr(de, "xfce"))
		result = "Xfce";
	else if (getenv("GNOME_DESKTOP_SESSION_ID") || strcasestr(de, "unity") || strcasestr(de, "gnome"))
		result = "GNOME";
	else if (strcasestr(de, "xfwm"))
		result = "Xfwm";
	else if (strcasestr(de, "openbox"))
		result = "Openbox";
	else if (strcasestr(de, "i3"))
		result = "i3";
	else if (strcasestr(de, "bspwm"))
		result = "bspwm";
	else if (strcasestr(de, "mutter"))
		result = "Mutter";
	else if (strcasestr(de, "sawfish"))
		result = "Sawfish";
	else if (strcasestr(de, "fluxbox"))
		result = "Fluxbox";
	else if (strcasestr(de, "icewm"))
		result = "IceWM";
	else if (strcasestr(de, "awesome"))
		result = "awesome";
	else if (strcasestr(de, "dwm"))
		result = "dwm";
	else if (de)
		result = de;
	else
		result = getenv("DESKTOP_SESSION");

	if (!result) return NULL;
	return line(result, false, mod);
}

module_output module_editor(module *mod) {
	char *editor_path = getenv("EDITOR");
	if (!editor_path) return NULL;

	char *editor = get_basename(editor_path);

	if (strcmp(editor, "nvim") == 0) editor = "neovim";

	return line(editor, false, mod);
}

module_output module_host(module *mod) {
	const char *product_name_file = "/sys/devices/virtual/dmi/id/product_name";
	const char *product_version_file = "/sys/devices/virtual/dmi/id/product_version";

	void *data1, *data2;
	size_t size1, size2;

	if (!read_filename(product_name_file, &data1, &size1)) return NULL;
	size1 = get_first_line(data1, size1);

	if (!read_filename(product_version_file, &data2, &size2)) return NULL;
	size2 = get_first_line(data2, size2);

	size_t str_len = size1 + size2 + 2;
	char *str = malloc(str_len);
	if (!str) {
		warn("malloc");
		return NULL;
	}
	memcpy(str, data1, size1);
	str[size1] = ' ';
	memcpy(str + size1 + 1, data2, size2);
	str[size1 + size2 + 1] = '\0';

	free(data1);
	free(data2);

	return line(str, true, mod);
}

module_output module_arch(module *mod) {
	struct utsname *un = get_utsname();
	if (!un) return NULL;
	return line(un->machine, false, mod);
}

module *modules = (module[]){
        {"username", "", module_username, false},
        {"hostname", "󰛳", module_hostname, false},
        {"header", NULL, module_header, true},
        {"line", NULL, module_line, true},
        {"os", "", module_os, true},
        {"kernel", "", module_kernel, true},
        {"uptime", "", module_uptime, true},
        {"shell", "", module_shell, true},
        {"ram", "󰍛", module_ram, true},
        {"swap", "󰓡", module_swap, true},
        {"de", "", module_de, true},
        {"editor", "", module_editor, true},
        {"host", "󰍹", module_host, true},
        {"arch", "", module_arch, true},
        {0}
};
