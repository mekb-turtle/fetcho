#ifndef MODULES_H
#define MODULES_H
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct colored_text {
	char *string;
	bool free;
	uint8_t fg_color;
	uint8_t bg_color;
	uint8_t flags;
} *module_output;

#define FLAG_BOLD (1 << 0)
#define FLAG_ITALIC (1 << 1)
#define FLAG_UNDERLINE (1 << 2)
#define FLAG_STRIKETHROUGH (1 << 3)
#define FLAG_FG_COLOR (1 << 4)
#define FLAG_BG_COLOR (1 << 5)
#define HAS_ANY_FLAG(bitmask, flag) ((bitmask & flag) == flag)
#define HAS_FLAG(bitmask, flag) ((bitmask & flag) == flag)

typedef struct module {
	char *name;
	module_output (*func)(struct module *);
	bool display_by_default;
} module;

extern module *modules;
#endif //MODULES_H
