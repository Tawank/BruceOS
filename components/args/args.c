#include "args.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/stdio.h"
#include "core_sdk/tty.h"

typedef enum {
    AP_OPT_FLAG,
    AP_OPT_STRING,
    AP_OPT_INT,
    AP_OPT_DOUBLE,
} ap_option_type_t;

typedef union {
    char *string;
    int integer;
    double real;
} ap_value_t;

typedef struct {
    char *names;
    char *helptext;
    ap_option_type_t type;
    ap_value_t fallback;
    ap_value_t *values;
    int value_count;
    int value_capacity;
    bool greedy;
} ap_option_t;

typedef struct {
    char *name;
    char *helptext;
    bool required;
} ap_positional_t;

typedef struct {
    char *names;
    struct ArgParser *parser;
} ap_command_t;

typedef union {
    max_align_t alignment;
    unsigned char bytes[8192];
} ap_arena_storage_t;

typedef struct {
    ap_arena_storage_t storage;
    size_t used;
} ap_arena_t;

struct ArgParser {
    ap_arena_t *arena;
    char *helptext;
    char *version;
    ap_option_t **options;
    int option_count;
    int option_capacity;
    ap_positional_t *positionals;
    int positional_count;
    int positional_capacity;
    char **parsed_args;
    int parsed_arg_count;
    int parsed_arg_capacity;
    ap_command_t *commands;
    int command_count;
    int command_capacity;
    ap_callback_t cmd_callback;
    int cmd_callback_exit_code;
    char *cmd_name;
    struct ArgParser *cmd_parser;
    struct ArgParser *root_parser;
    struct ArgParser *parent;
    const char *display_name;
    char *zeroth_root_arg;
    ap_status_t status;
    bool enable_help_command;
    bool first_pos_arg_ends_option_parsing;
    bool all_args_as_pos_args;
    bool allow_extra_args;
    bool unknown_options_as_args;
};

static void *ap_alloc(ArgParser *parser, size_t size) {
    ArgParser *root = parser != NULL && parser->root_parser != NULL ? parser->root_parser : parser;
    if (root == NULL || root->arena == NULL || size == 0) return NULL;
    size_t alignment = _Alignof(max_align_t);
    size_t offset = (root->arena->used + alignment - 1) & ~(alignment - 1);
    if (offset > sizeof(root->arena->storage.bytes) || size > sizeof(root->arena->storage.bytes) - offset) {
        return NULL;
    }
    void *allocation = root->arena->storage.bytes + offset;
    root->arena->used = offset + size;
    return allocation;
}

static void *ap_calloc(ArgParser *parser, size_t count, size_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *allocation = ap_alloc(parser, total);
    return allocation != NULL ? memset(allocation, 0, total) : NULL;
}

static char *ap_strdup(ArgParser *parser, const char *value) {
    if (value == NULL) return NULL;
    size_t size = strlen(value) + 1;
    char *copy = ap_alloc(parser, size);
    return copy != NULL ? memcpy(copy, value, size) : NULL;
}

static bool ap_grow(ArgParser *parser, void **items, int *capacity, int count, size_t item_size) {
    if (count < *capacity) return true;
    int new_capacity = *capacity < 4 ? 4 : *capacity * 2;
    void *grown = ap_alloc(parser, (size_t)new_capacity * item_size);
    if (grown == NULL) return false;
    if (*items != NULL && count > 0) memcpy(grown, *items, (size_t)count * item_size);
    *items = grown;
    *capacity = new_capacity;
    return true;
}

static ArgParser *ap_root(ArgParser *parser) {
    return parser != NULL && parser->root_parser != NULL ? parser->root_parser : parser;
}

static void ap_set_status(ArgParser *parser, ap_status_t status) {
    if (parser == NULL) return;
    parser->status = status;
    ArgParser *root = ap_root(parser);
    if (root != NULL) root->status = status;
}

static void ap_fail(ArgParser *parser, ap_status_t status, const char *format, ...) {
    if (parser == NULL || ap_get_status(parser) != AP_STATUS_OK) return;
    ap_set_status(parser, status);
    stdio__printf("error: ");
    va_list args;
    va_start(args, format);
    stdio__vprintf(format, args);
    va_end(args);
    stdio__printf("\n");
}

static bool ap_name_matches(const char *names, const char *name) {
    if (names == NULL || name == NULL || name[0] == '\0') return false;
    size_t name_len = strlen(name);
    const char *cursor = names;
    while (*cursor != '\0') {
        while (*cursor == ' ') cursor++;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ') cursor++;
        if ((size_t)(cursor - start) == name_len && memcmp(start, name, name_len) == 0) return true;
    }
    return false;
}

static const char *ap_primary_name(const char *names, size_t *out_len) {
    const char *start = names != NULL ? names : "";
    while (*start == ' ') start++;
    const char *end = start;
    while (*end != '\0' && *end != ' ') end++;
    if (out_len != NULL) *out_len = (size_t)(end - start);
    return start;
}

static ap_option_t *ap_find_option(ArgParser *parser, const char *name) {
    if (parser == NULL) return NULL;
    for (int i = 0; i < parser->option_count; ++i) {
        if (ap_name_matches(parser->options[i]->names, name)) return parser->options[i];
    }
    return NULL;
}

static ap_command_t *ap_find_command(ArgParser *parser, const char *name) {
    if (parser == NULL) return NULL;
    for (int i = 0; i < parser->command_count; ++i) {
        if (ap_name_matches(parser->commands[i].names, name)) return &parser->commands[i];
    }
    return NULL;
}

static bool ap_parse_int(ArgParser *parser, const char *text, int *out_value) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 0);
    if (errno == ERANGE || value < INT_MIN || value > INT_MAX) {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "'%s' is out of range", text);
        return false;
    }
    if (end == text || *end != '\0') {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "cannot parse '%s' as an integer", text);
        return false;
    }
    *out_value = (int)value;
    return true;
}

static bool ap_parse_double(ArgParser *parser, const char *text, double *out_value) {
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno == ERANGE) {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "'%s' is out of range", text);
        return false;
    }
    if (end == text || *end != '\0') {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "cannot parse '%s' as a floating-point value", text);
        return false;
    }
    *out_value = value;
    return true;
}

ArgParser *ap_new_parser(void) {
    ap_arena_t *arena = memory__calloc(1, sizeof(*arena));
    if (arena == NULL) return NULL;
    ArgParser *parser = (ArgParser *)arena->storage.bytes;
    arena->used = sizeof(*parser);
    memset(parser, 0, sizeof(*parser));
    parser->arena = arena;
    parser->root_parser = parser;
    parser->status = AP_STATUS_OK;
    return parser;
}

void ap_free(ArgParser *parser) {
    if (parser == NULL) return;
    if (parser->parent == NULL) memory__free(parser->arena);
}

static void ap_replace_text(ArgParser *parser, char **field, const char *value) {
    if (parser == NULL) return;
    char *copy = ap_strdup(parser, value);
    if (value != NULL && copy == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return;
    }
    *field = copy;
}

void ap_set_helptext(ArgParser *parser, const char *helptext) {
    if (parser != NULL) ap_replace_text(parser, &parser->helptext, helptext);
}

char *ap_get_helptext(ArgParser *parser) { return parser != NULL ? parser->helptext : NULL; }

void ap_set_version(ArgParser *parser, const char *version) {
    if (parser != NULL) ap_replace_text(parser, &parser->version, version);
}

char *ap_get_version(ArgParser *parser) { return parser != NULL ? parser->version : NULL; }

void ap_first_pos_arg_ends_option_parsing(ArgParser *parser) {
    if (parser != NULL) parser->first_pos_arg_ends_option_parsing = true;
}

void ap_all_args_as_pos_args(ArgParser *parser) {
    if (parser != NULL) parser->all_args_as_pos_args = true;
}

void ap_allow_extra_args(ArgParser *parser) {
    if (parser != NULL) parser->allow_extra_args = true;
}

void ap_unknown_options_as_args(ArgParser *parser) {
    if (parser != NULL) parser->unknown_options_as_args = true;
}

static void ap_add_option(ArgParser *parser, const char *names, ap_option_type_t type, ap_value_t fallback, bool greedy) {
    if (parser == NULL || names == NULL || names[0] == '\0' || ap_get_status(parser) != AP_STATUS_OK) return;
    ap_option_t *option = ap_calloc(parser, 1, sizeof(*option));
    if (option == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return;
    }
    option->names = ap_strdup(parser, names);
    if (option->names == NULL ||
        !ap_grow(parser, (void **)&parser->options, &parser->option_capacity, parser->option_count, sizeof(*parser->options))) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return;
    }
    option->type = type;
    option->fallback = fallback;
    option->greedy = greedy;
    parser->options[parser->option_count++] = option;
}

void ap_add_flag(ArgParser *parser, const char *name) {
    ap_add_option(parser, name, AP_OPT_FLAG, (ap_value_t){0}, false);
}

void ap_add_str_opt(ArgParser *parser, const char *name, const char *fallback) {
    char *fallback_copy = ap_strdup(parser, fallback);
    if (fallback != NULL && fallback_copy == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return;
    }
    ap_add_option(parser, name, AP_OPT_STRING, (ap_value_t){.string = fallback_copy}, false);
}

void ap_add_int_opt(ArgParser *parser, const char *name, int fallback) {
    ap_add_option(parser, name, AP_OPT_INT, (ap_value_t){.integer = fallback}, false);
}

void ap_add_dbl_opt(ArgParser *parser, const char *name, double fallback) {
    ap_add_option(parser, name, AP_OPT_DOUBLE, (ap_value_t){.real = fallback}, false);
}

void ap_add_greedy_str_opt(ArgParser *parser, const char *name) {
    ap_add_str_opt(parser, name, "");
    ap_option_t *option = ap_find_option(parser, name);
    if (option != NULL) option->greedy = true;
}

void ap_set_opt_help(ArgParser *parser, const char *name, const char *helptext) {
    ap_option_t *option = ap_find_option(parser, name);
    if (option != NULL) ap_replace_text(parser, &option->helptext, helptext);
}

static bool ap_append_option_value(ArgParser *parser, ap_option_t *option, const char *text) {
    ap_value_t value = {0};
    if (option->type == AP_OPT_STRING) value.string = (char *)text;
    else if (option->type == AP_OPT_INT && !ap_parse_int(parser, text, &value.integer)) return false;
    else if (option->type == AP_OPT_DOUBLE && !ap_parse_double(parser, text, &value.real)) return false;
    if (!ap_grow(parser, (void **)&option->values, &option->value_capacity, option->value_count, sizeof(*option->values))) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return false;
    }
    option->values[option->value_count++] = value;
    return true;
}

int ap_count(ArgParser *parser, const char *name) {
    ap_option_t *option = ap_find_option(parser, name);
    return option != NULL ? option->value_count : 0;
}

bool ap_found(ArgParser *parser, const char *name) { return ap_count(parser, name) > 0; }

char *ap_get_str_value(ArgParser *parser, const char *name) {
    ap_option_t *option = ap_find_option(parser, name);
    if (option == NULL || option->type != AP_OPT_STRING) return NULL;
    return option->value_count > 0 ? option->values[option->value_count - 1].string : option->fallback.string;
}

char *ap_get_str_value_at_index(ArgParser *parser, const char *name, int index) {
    ap_option_t *option = ap_find_option(parser, name);
    return option != NULL && option->type == AP_OPT_STRING && index >= 0 && index < option->value_count
               ? option->values[index].string
               : NULL;
}

int ap_get_int_value(ArgParser *parser, const char *name) {
    ap_option_t *option = ap_find_option(parser, name);
    if (option == NULL || option->type != AP_OPT_INT) return 0;
    return option->value_count > 0 ? option->values[option->value_count - 1].integer : option->fallback.integer;
}

int ap_get_int_value_at_index(ArgParser *parser, const char *name, int index) {
    ap_option_t *option = ap_find_option(parser, name);
    return option != NULL && option->type == AP_OPT_INT && index >= 0 && index < option->value_count
               ? option->values[index].integer
               : 0;
}

double ap_get_dbl_value(ArgParser *parser, const char *name) {
    ap_option_t *option = ap_find_option(parser, name);
    if (option == NULL || option->type != AP_OPT_DOUBLE) return 0.0;
    return option->value_count > 0 ? option->values[option->value_count - 1].real : option->fallback.real;
}

double ap_get_dbl_value_at_index(ArgParser *parser, const char *name, int index) {
    ap_option_t *option = ap_find_option(parser, name);
    return option != NULL && option->type == AP_OPT_DOUBLE && index >= 0 && index < option->value_count
               ? option->values[index].real
               : 0.0;
}

static void *ap_copy_option_values(ArgParser *parser, const char *name, ap_option_type_t type, size_t size) {
    ap_option_t *option = ap_find_option(parser, name);
    if (option == NULL || option->type != type || option->value_count == 0) return NULL;
    void *copy = memory__malloc((size_t)option->value_count * size);
    if (copy == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return NULL;
    }
    for (int i = 0; i < option->value_count; ++i) {
        if (type == AP_OPT_STRING) ((char **)copy)[i] = option->values[i].string;
        else if (type == AP_OPT_INT) ((int *)copy)[i] = option->values[i].integer;
        else ((double *)copy)[i] = option->values[i].real;
    }
    return copy;
}

char **ap_get_str_values(ArgParser *parser, const char *name) {
    return ap_copy_option_values(parser, name, AP_OPT_STRING, sizeof(char *));
}

int *ap_get_int_values(ArgParser *parser, const char *name) {
    return ap_copy_option_values(parser, name, AP_OPT_INT, sizeof(int));
}

double *ap_get_dbl_values(ArgParser *parser, const char *name) {
    return ap_copy_option_values(parser, name, AP_OPT_DOUBLE, sizeof(double));
}

static void ap_add_positional(ArgParser *parser, const char *name, const char *helptext, bool required) {
    if (parser == NULL || name == NULL || name[0] == '\0' || ap_get_status(parser) != AP_STATUS_OK) return;
    for (int i = 0; i < parser->positional_count; ++i) {
        if (strcmp(parser->positionals[i].name, name) == 0) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "positional argument '%s' is already registered", name);
            return;
        }
        if (required && !parser->positionals[i].required) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "required positional '%s' cannot follow an optional positional", name);
            return;
        }
    }
    if (!ap_grow(
            parser,
            (void **)&parser->positionals,
            &parser->positional_capacity,
            parser->positional_count,
            sizeof(*parser->positionals)
        )) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return;
    }
    ap_positional_t *positional = &parser->positionals[parser->positional_count];
    positional->name = ap_strdup(parser, name);
    positional->helptext = ap_strdup(parser, helptext);
    positional->required = required;
    if (positional->name == NULL || (helptext != NULL && positional->helptext == NULL)) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return;
    }
    parser->positional_count++;
}

void ap_add_required_arg(ArgParser *parser, const char *name, const char *helptext) {
    ap_add_positional(parser, name, helptext, true);
}

void ap_add_optional_arg(ArgParser *parser, const char *name, const char *helptext) {
    ap_add_positional(parser, name, helptext, false);
}

char *ap_get_arg(ArgParser *parser, const char *name) {
    if (parser == NULL || name == NULL) return NULL;
    for (int i = 0; i < parser->positional_count; ++i) {
        if (strcmp(parser->positionals[i].name, name) == 0)
            return i < parser->parsed_arg_count ? parser->parsed_args[i] : NULL;
    }
    return NULL;
}

bool ap_has_args(ArgParser *parser) { return parser != NULL && parser->parsed_arg_count > 0; }
int ap_count_args(ArgParser *parser) { return parser != NULL ? parser->parsed_arg_count : 0; }

char *ap_get_arg_at_index(ArgParser *parser, int index) {
    return parser != NULL && index >= 0 && index < parser->parsed_arg_count ? parser->parsed_args[index] : NULL;
}

char **ap_get_args(ArgParser *parser) {
    if (parser == NULL || parser->parsed_arg_count == 0) return NULL;
    size_t size = (size_t)parser->parsed_arg_count * sizeof(char *);
    char **copy = memory__malloc(size);
    if (copy == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return NULL;
    }
    return memcpy(copy, parser->parsed_args, size);
}

int *ap_get_args_as_ints(ArgParser *parser) {
    if (parser == NULL || parser->parsed_arg_count == 0) return NULL;
    int *values = memory__malloc((size_t)parser->parsed_arg_count * sizeof(*values));
    if (values == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return NULL;
    }
    for (int i = 0; i < parser->parsed_arg_count; ++i) {
        if (!ap_parse_int(parser, parser->parsed_args[i], &values[i])) {
            memory__free(values);
            return NULL;
        }
    }
    return values;
}

double *ap_get_args_as_doubles(ArgParser *parser) {
    if (parser == NULL || parser->parsed_arg_count == 0) return NULL;
    double *values = memory__malloc((size_t)parser->parsed_arg_count * sizeof(*values));
    if (values == NULL) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return NULL;
    }
    for (int i = 0; i < parser->parsed_arg_count; ++i) {
        if (!ap_parse_double(parser, parser->parsed_args[i], &values[i])) {
            memory__free(values);
            return NULL;
        }
    }
    return values;
}

ArgParser *ap_new_cmd(ArgParser *parent, const char *names) {
    if (parent == NULL || names == NULL || names[0] == '\0' || ap_get_status(parent) != AP_STATUS_OK) return NULL;
    ArgParser *child = ap_calloc(parent, 1, sizeof(*child));
    if (child == NULL) {
        ap_set_status(parent, AP_STATUS_NO_MEMORY);
        return NULL;
    }
    child->arena = ap_root(parent)->arena;
    child->parent = parent;
    child->root_parser = ap_root(parent);
    child->status = AP_STATUS_OK;
    char *names_copy = ap_strdup(parent, names);
    if (names_copy == NULL ||
        !ap_grow(parent, (void **)&parent->commands, &parent->command_capacity, parent->command_count, sizeof(*parent->commands))) {
        ap_set_status(parent, AP_STATUS_NO_MEMORY);
        return NULL;
    }
    child->display_name = names_copy;
    parent->commands[parent->command_count++] = (ap_command_t){.names = names_copy, .parser = child};
    parent->enable_help_command = true;
    return child;
}

void ap_set_cmd_callback(ArgParser *parser, ap_callback_t callback) {
    if (parser != NULL) parser->cmd_callback = callback;
}

bool ap_found_cmd(ArgParser *parser) { return parser != NULL && parser->cmd_parser != NULL; }
char *ap_get_cmd_name(ArgParser *parser) { return parser != NULL ? parser->cmd_name : NULL; }
ArgParser *ap_get_cmd_parser(ArgParser *parser) { return parser != NULL ? parser->cmd_parser : NULL; }
int ap_get_cmd_exit_code(ArgParser *parser) { return parser != NULL ? parser->cmd_callback_exit_code : 0; }

void ap_enable_help_command(ArgParser *parser, bool enable) {
    if (parser != NULL) parser->enable_help_command = enable;
}

ArgParser *ap_get_parent(ArgParser *parser) { return parser != NULL ? parser->parent : NULL; }

static void ap_print_usage_path(ArgParser *parser) {
    if (parser->parent != NULL) {
        ap_print_usage_path(parser->parent);
        size_t len = 0;
        const char *name = ap_primary_name(parser->display_name, &len);
        stdio__printf(" %.*s", (int)len, name);
    } else {
        stdio__printf("%s", parser->zeroth_root_arg != NULL ? parser->zeroth_root_arg : "command");
    }
}

/* Appends formatted text to `buf` (capacity `cap`) starting at `*len` bytes
 * already used, the same way repeated snprintf() calls compose a string --
 * `*len` tracks the length that would have been written even past `cap`
 * (like snprintf's own return value), so overflowing calls are silently
 * measured rather than corrupting anything, and a caller only interested in
 * the final length can pass a buffer that's too small on purpose. */
static void ap_buf_appendf(char *buf, size_t cap, size_t *len, const char *format, ...) {
    size_t offset = *len < cap ? *len : cap;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(buf + offset, cap - offset, format, args);
    va_end(args);
    if (n > 0) *len += (size_t)n;
}

/* Appends `names` (a parser's/option's space-separated alias list) to `buf`
 * as a comma-separated list. `dash_prefixed` picks "-"/"--" per token
 * (options, where the token length says which) or no prefix at all
 * (subcommand names, which aren't typed with a dash). */
static void ap_format_names(char *buf, size_t cap, size_t *len, const char *names, bool dash_prefixed) {
    const char *cursor = names;
    bool first = true;
    while (cursor != NULL && *cursor != '\0') {
        while (*cursor == ' ') cursor++;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ') cursor++;
        if (cursor == start) break;
        const char *prefix = !dash_prefixed ? "" : (cursor - start == 1 ? "-" : "--");
        ap_buf_appendf(buf, cap, len, "%s%s%.*s", first ? "" : ", ", prefix, (int)(cursor - start), start);
        first = false;
    }
}

/* The next three build one help-row's left-hand label ("name (required)",
 * "-x, --long <value>", ...) into `buf` and return its length, with no
 * leading indent and no trailing gap or helptext - just the label itself,
 * so ap_print_help() below can both measure it (for column alignment) and
 * print it (once padded) from the exact same formatting logic. */
static size_t ap_format_command_label(char *buf, size_t cap, const ap_command_t *command) {
    size_t len = 0;
    ap_format_names(buf, cap, &len, command->names, false);
    for (int j = 0; j < command->parser->positional_count; ++j) {
        ap_positional_t *positional = &command->parser->positionals[j];
        ap_buf_appendf(buf, cap, &len, positional->required ? " <%s>" : " [%s]", positional->name);
    }
    if (command->parser->allow_extra_args) ap_buf_appendf(buf, cap, &len, " [args...]");
    if (command->parser->command_count > 0) ap_buf_appendf(buf, cap, &len, " <command>");
    return len;
}

static size_t ap_format_positional_label(char *buf, size_t cap, const ap_positional_t *positional) {
    size_t len = 0;
    ap_buf_appendf(buf, cap, &len, "%s%s", positional->name, positional->required ? " (required)" : "");
    return len;
}

static size_t ap_format_option_label(char *buf, size_t cap, const ap_option_t *option) {
    size_t len = 0;
    ap_format_names(buf, cap, &len, option->names, true);
    if (option->type != AP_OPT_FLAG) ap_buf_appendf(buf, cap, &len, " <value>");
    return len;
}

/* Aligned mode's line-wrap target: a fenced ```code block``` on GitHub never
 * soft-wraps a long line the way a real terminal (or an ordinary Markdown
 * paragraph outside the fence) does -- it just grows a horizontal scrollbar,
 * which is exactly what a long helptext string used to do here (`nc`'s -e
 * and -q options, `nmap`'s --sU, the top-level command blurbs, ...). Only
 * relevant to aligned mode - unaligned/on-device output already gets
 * soft-wrapped for free by the terminal it's rendered in, and hard-wrapping
 * it here too would just fight over the width with whatever that terminal
 * actually is. 100 columns is a common "reads fine without scrolling"
 * width for a monospace code block; there's nothing sharper to target since
 * GitHub doesn't publish one. */
#define AP_HELP_WRAP_WIDTH 100u
/* Aligned mode's shared label column, capped: `label_width` below (computed
 * per ap_print_help() call from the widest label actually present) would,
 * uncapped, let one oddball long label - a positional's "(required)" suffix,
 * a "-c <value>" placeholder, whatever - drag every short flag's column out
 * with it. Real man pages don't do that either: BSD nc(1)'s -I/-i take a
 * value name ("length", "interval") too wide for its option list's column,
 * so they get their own line - see ap_print_row(). A label under this cap
 * still gets the normal shared-column treatment; this only kicks in for the
 * minority that would've blown the column out for everyone else. Paired with
 * ap_print_row()'s 4-space indent, this keeps the description column at 4+8
 * (plus the 2-space gap) - tight, matching the pasted nc(1) example instead
 * of trailing off toward the far side of the screen. */
#define AP_HELP_LABEL_CAP 8u

/* Prints `text` word-wrapped so no line exceeds `width` columns, indenting
 * every line after the first by `indent` spaces (the first line isn't
 * indented here - the caller has usually already positioned the cursor
 * there, e.g. ap_print_row()'s label + gap). Never emits a trailing
 * newline; the caller adds one. Whitespace-only breaking (spaces and
 * newlines both treated as breakable, collapsed): no current helptext
 * embeds a deliberate line break, so there's nothing to preserve, and it
 * keeps this simple. A single word longer than the whole content width is
 * still printed whole rather than split mid-word - a hyphenated URL or
 * path is more readable overflowing one line than chopped arbitrarily. */
static void ap_print_wrapped(const char *text, size_t indent, size_t width) {
    if (text == NULL || text[0] == '\0') return;
    size_t content_width = width > indent + 20 ? width - indent : 20;
    size_t col = 0;
    bool first_word = true;
    const char *cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\n') cursor++;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\n') cursor++;
        size_t word_len = (size_t)(cursor - start);
        if (word_len == 0) break;
        if (!first_word && col + 1 + word_len > content_width) {
            stdio__printf("\n%*s", (int)indent, "");
            col = 0;
            first_word = true;
        }
        if (!first_word) {
            stdio__printf(" ");
            col += 1;
        }
        stdio__printf("%.*s", (int)word_len, start);
        col += word_len;
        first_word = false;
    }
}

/* See args.h's doc comment - the same aligned/unaligned split ap_print_help()
 * makes for its own command-level helptext blurb, exposed for a command
 * that builds its own --help text by hand instead of registering it with
 * ArgParser. */
void ap_print_wrapped_help(const char *text) {
    if (text == NULL || text[0] == '\0') return;
    if (tty__isatty()) {
        stdio__printf("%s", text);
        return;
    }
    ap_print_wrapped(text, 0, AP_HELP_WRAP_WIDTH);
}

/* Prints one already-built help row. Unaligned (on-device) mode is exactly
 * what it's always been: two-space indent, a fixed two-space gap, unwrapped
 * helptext - a real terminal handles wrapping itself, and the small
 * physical display has no room for anything wider anyway.
 *
 * Aligned mode instead follows a classic man page's option list: four-space
 * indent, helptext starting at a shared column (`label_width` - the
 * widest label anywhere in this ap_print_help() call, capped at
 * AP_HELP_LABEL_CAP - plus indent plus a minimum two-space gap), wrapped
 * (AP_HELP_WRAP_WIDTH) with continuation lines indented to that same
 * column. A label wider than the cap doesn't get squeezed into that gap or
 * drag the column out for every other row - it's printed alone, then the
 * helptext starts on its own line at the same column, same as real man
 * pages do for a long tag (BSD nc(1)'s "-I length"/"-i interval" among
 * plain single-char flags). */
static void ap_print_row(const char *label, const char *helptext, bool aligned, size_t label_width) {
    size_t indent = aligned ? 4 : 2;
    stdio__printf("%*s%s", (int)indent, "", label);
    if (helptext == NULL || helptext[0] == '\0') {
        stdio__printf("\n");
        return;
    }
    if (!aligned) {
        stdio__printf("  %s\n", helptext);
        return;
    }
    size_t label_len = strlen(label);
    size_t desc_column = indent + label_width + 2;
    if (label_len > label_width) stdio__printf("\n%*s", (int)desc_column, "");
    else stdio__printf("%*s", (int)(desc_column - indent - label_len), "");
    ap_print_wrapped(helptext, desc_column, AP_HELP_WRAP_WIDTH);
    stdio__printf("\n");
}

void ap_print_help(ArgParser *parser) {
    if (parser == NULL) return;
    stdio__printf("Usage: ");
    ap_print_usage_path(parser);
    if (parser->command_count > 0) stdio__printf(" <command>");
    for (int i = 0; i < parser->positional_count; ++i) {
        stdio__printf(parser->positionals[i].required ? " <%s>" : " [%s]", parser->positionals[i].name);
    }
    if (parser->allow_extra_args) stdio__printf(" [args...]");
    if (parser->option_count > 0) stdio__printf(" [options]");
    stdio__printf("\n");

    /* Every "label  helptext" row below (Commands/Arguments/Options, plus
     * the built-in -h/-v lines), and this command's own helptext blurb just
     * below, used to separate label and text with a literal '\t' - which a
     * real terminal lands at whatever tab stop comes after the label, a
     * different column depending how long the label was, so two labels of
     * different lengths never lined their helptext up. Replaced with a
     * column computed from the widest label in this help output (aligned
     * mode), or a plain two-space gap (unaligned) with no attempt at
     * alignment at all.
     *
     * Which one applies is decided the same way man_app.c's --line-marker
     * decides live-terminal vs. captured output: tty__isatty(). The
     * device's own console is a real tty with a small physical display, no
     * room to spare on a wide alignment column - aligned mode is for
     * anything reading this over a non-tty pipe (a host capture, e.g.
     * `man --gen-md`'s per-command `<command> --help` capture, or a plain
     * `<command> --help > file`), where terminal width isn't a scarce
     * resource. Aligned mode also word-wraps long helptext (AP_HELP_WRAP_WIDTH
     * above ap_print_row()) - a fenced code block on GitHub never soft-wraps
     * a long line, it just grows a horizontal scrollbar, which is exactly
     * where that doc ends up rendered. */
    bool aligned = !tty__isatty();

    if (parser->helptext != NULL && parser->helptext[0] != '\0') {
        stdio__printf("\n");
        ap_print_wrapped_help(parser->helptext);
        stdio__printf("\n");
    }

    size_t label_width = 0;
    char label[128];
    /* Same opt-out ap_parse() itself already applies when deciding whether a
     * bare "-h" means "show help" (see its own ap_find_option(parser, "h")
     * check): a command that has claimed "h" for its own option (`top`'s and
     * `free`'s "-h" for human-readable sizes, e.g.) keeps its own listing
     * for "h" and this becomes --help-only, instead of the two colliding
     * "-h, --help" / "-h  <something else>" rows that used to both list
     * plain "-h". */
    const char *help_label = ap_find_option(parser, "h") == NULL ? "-h, --help" : "--help";
    const char *version_label = parser->version == NULL                    ? NULL
                                 : ap_find_option(parser, "v") == NULL ? "-v, --version"
                                                                        : "--version";

    if (aligned) {
        for (int i = 0; i < parser->command_count; ++i) {
            size_t len = ap_format_command_label(label, sizeof(label), &parser->commands[i]);
            if (len > label_width) label_width = len;
        }
        for (int i = 0; i < parser->positional_count; ++i) {
            size_t len = ap_format_positional_label(label, sizeof(label), &parser->positionals[i]);
            if (len > label_width) label_width = len;
        }
        for (int i = 0; i < parser->option_count; ++i) {
            size_t len = ap_format_option_label(label, sizeof(label), parser->options[i]);
            if (len > label_width) label_width = len;
        }
        if (strlen(help_label) > label_width) label_width = strlen(help_label);
        if (version_label != NULL && strlen(version_label) > label_width) label_width = strlen(version_label);
        if (label_width > AP_HELP_LABEL_CAP) label_width = AP_HELP_LABEL_CAP;
    }

    /* Aligned mode separates every row with a blank line, man-page style
     * (see ap_print_row()'s own comment); unaligned/on-device output stays
     * exactly as tight as it's always been. `first` resets per section
     * (Commands/Arguments/Options) - each section's own header line already
     * separates it from the last row of the section before it, so there's
     * nothing to add before a section's own first row. */
    bool first;

    if (parser->command_count > 0) {
        stdio__printf("\nCommands:\n");
        first = true;
        for (int i = 0; i < parser->command_count; ++i) {
            if (aligned && !first) stdio__printf("\n");
            first = false;
            ap_command_t *command = &parser->commands[i];
            ap_format_command_label(label, sizeof(label), command);
            ap_print_row(label, command->parser->helptext, aligned, label_width);
        }
    }

    if (parser->positional_count > 0) {
        stdio__printf("\nArguments:\n");
        first = true;
        for (int i = 0; i < parser->positional_count; ++i) {
            if (aligned && !first) stdio__printf("\n");
            first = false;
            ap_positional_t *positional = &parser->positionals[i];
            ap_format_positional_label(label, sizeof(label), positional);
            ap_print_row(label, positional->helptext, aligned, label_width);
        }
    }

    stdio__printf("\nOptions:\n");
    first = true;
    for (int i = 0; i < parser->option_count; ++i) {
        if (aligned && !first) stdio__printf("\n");
        first = false;
        ap_option_t *option = parser->options[i];
        ap_format_option_label(label, sizeof(label), option);
        ap_print_row(label, option->helptext, aligned, label_width);
    }
    if (aligned && !first) stdio__printf("\n");
    ap_print_row(help_label, "Show this help", aligned, label_width);
    if (version_label != NULL) {
        if (aligned) stdio__printf("\n");
        ap_print_row(version_label, "Show version", aligned, label_width);
    }
}

static bool ap_append_positional(ArgParser *parser, char *value) {
    if (!ap_grow(
            parser,
            (void **)&parser->parsed_args,
            &parser->parsed_arg_capacity,
            parser->parsed_arg_count,
            sizeof(*parser->parsed_args)
        )) {
        ap_set_status(parser, AP_STATUS_NO_MEMORY);
        return false;
    }
    parser->parsed_args[parser->parsed_arg_count++] = value;
    return true;
}

static bool ap_handle_option(ArgParser *parser, const char *name, const char *inline_value, int argc, char **argv, int *index) {
    ap_option_t *option = ap_find_option(parser, name);
    if (option == NULL) {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "'%s' is not a recognised option", argv[*index]);
        return false;
    }
    if (option->type == AP_OPT_FLAG) {
        if (inline_value != NULL) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "flag '%s' does not accept a value", name);
            return false;
        }
        option->value_count++;
        return true;
    }
    const char *value = inline_value;
    if (value == NULL) {
        if (*index + 1 >= argc) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "missing value for '%s'", name);
            return false;
        }
        value = argv[++*index];
    }
    if (value[0] == '\0') {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "empty value for '%s'", name);
        return false;
    }
    if (!ap_append_option_value(parser, option, value)) return false;
    if (option->greedy) {
        while (*index + 1 < argc) {
            if (!ap_append_option_value(parser, option, argv[++*index])) return false;
        }
    }
    return true;
}

static bool ap_handle_short_options(ArgParser *parser, const char *text, int argc, char **argv, int *index) {
    for (size_t offset = 0; text[offset] != '\0'; ++offset) {
        char name[2] = {text[offset], '\0'};
        ap_option_t *option = ap_find_option(parser, name);
        if (option == NULL) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "-%s is not a recognised option", text);
            return false;
        }
        if (option->type == AP_OPT_FLAG) option->value_count++;
        else {
            if (text[offset + 1] != '\0') {
                const char *value = text + offset + 1 + (text[offset + 1] == '=' ? 1 : 0);
                return value[0] != '\0' && ap_append_option_value(parser, option, value);
            }
            return ap_handle_option(parser, name, NULL, argc, argv, index);
        }
    }
    return true;
}

static bool ap_validate_positionals(ArgParser *parser) {
    if (parser->positional_count == 0) {
        if (parser->parsed_arg_count == 0 || parser->all_args_as_pos_args || parser->allow_extra_args) return true;
        ap_fail(
            parser,
            AP_STATUS_INVALID_ARGUMENT,
            parser->command_count > 0 ? "'%s' is not a recognised command" : "unexpected argument '%s'",
            parser->parsed_args[0]
        );
        return false;
    }
    if (!parser->allow_extra_args && parser->parsed_arg_count > parser->positional_count) {
        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "unexpected argument '%s'", parser->parsed_args[parser->positional_count]);
        return false;
    }
    for (int i = 0; i < parser->positional_count; ++i) {
        if (parser->positionals[i].required && i >= parser->parsed_arg_count) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "missing required argument <%s>", parser->positionals[i].name);
            return false;
        }
    }
    return true;
}

static bool ap_parse_level(ArgParser *parser, int argc, char **argv, int start) {
    bool options_enabled = !parser->all_args_as_pos_args;
    for (int i = start; i < argc && ap_get_status(parser) == AP_STATUS_OK; ++i) {
        char *arg = argv[i];
        if (arg == NULL) {
            ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "argv contains a null argument");
            return false;
        }
        if (options_enabled && strcmp(arg, "--") == 0) {
            options_enabled = false;
            continue;
        }
        /* "-h" is a --help shortcut only when the command hasn't claimed its
         * own "h" option (see bnu_ls_app_main's "-h" for human-readable
         * sizes) -- same opt-out shape as "-v" below deferring to the
         * command when it wants "-v" for something other than --version.
         * "--help" always means help; it has no such escape hatch. */
        if (options_enabled && strcmp(arg, "--help") == 0) {
            ap_print_help(parser);
            ap_set_status(parser, AP_STATUS_HELP);
            return false;
        }
        if (options_enabled && strcmp(arg, "-h") == 0 && ap_find_option(parser, "h") == NULL) {
            ap_print_help(parser);
            ap_set_status(parser, AP_STATUS_HELP);
            return false;
        }
        if (options_enabled && parser->version != NULL && strcmp(arg, "--version") == 0) {
            stdio__printf("%s\n", parser->version);
            ap_set_status(parser, AP_STATUS_VERSION);
            return false;
        }
        if (options_enabled && parser->version != NULL && strcmp(arg, "-v") == 0 &&
            ap_find_option(parser, "v") == NULL) {
            stdio__printf("%s\n", parser->version);
            ap_set_status(parser, AP_STATUS_VERSION);
            return false;
        }
        if (options_enabled && arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            const char *name = arg + 2;
            const char *equals = strchr(name, '=');
            size_t name_len = equals != NULL ? (size_t)(equals - name) : strlen(name);
            bool known = false;
            for (int option_index = 0; option_index < parser->option_count && !known; ++option_index) {
                const char *option_names = parser->options[option_index]->names;
                const char *cursor = option_names;
                while (*cursor != '\0') {
                    while (*cursor == ' ') cursor++;
                    const char *start_name = cursor;
                    while (*cursor != '\0' && *cursor != ' ') cursor++;
                    known = (size_t)(cursor - start_name) == name_len && memcmp(start_name, name, name_len) == 0;
                    if (known) break;
                }
            }
            if (!known && parser->unknown_options_as_args) {
                if (!ap_append_positional(parser, arg)) return false;
                continue;
            }
            if (equals != NULL) {
                size_t len = (size_t)(equals - name);
                char *copy = ap_alloc(parser, len + 1);
                if (copy == NULL) {
                    ap_set_status(parser, AP_STATUS_NO_MEMORY);
                    return false;
                }
                memcpy(copy, name, len);
                copy[len] = '\0';
                bool ok = ap_handle_option(parser, copy, equals + 1, argc, argv, &i);
                if (!ok) return false;
            } else if (!ap_handle_option(parser, name, NULL, argc, argv, &i)) return false;
            continue;
        }
        if (options_enabled && arg[0] == '-' && arg[1] != '\0' && !isdigit((unsigned char)arg[1])) {
            char short_name[2] = {arg[1], '\0'};
            if (parser->unknown_options_as_args && ap_find_option(parser, short_name) == NULL) {
                if (!ap_append_positional(parser, arg)) return false;
                continue;
            }
            if (!ap_handle_short_options(parser, arg + 1, argc, argv, &i)) return false;
            continue;
        }
        if (parser->parsed_arg_count == 0) {
            ap_command_t *command = ap_find_command(parser, arg);
            if (command != NULL) {
                parser->cmd_name = arg;
                parser->cmd_parser = command->parser;
                if (!ap_parse_level(command->parser, argc, argv, i + 1)) return false;
                if (command->parser->cmd_callback != NULL) {
                    parser->cmd_callback_exit_code = command->parser->cmd_callback(arg, command->parser);
                }
                return true;
            }
            if (parser->enable_help_command && strcmp(arg, "help") == 0) {
                if (i + 1 < argc) {
                    ap_command_t *help_command = ap_find_command(parser, argv[i + 1]);
                    if (help_command == NULL) {
                        ap_fail(parser, AP_STATUS_INVALID_ARGUMENT, "'%s' is not a recognised command", argv[i + 1]);
                        return false;
                    }
                    ap_print_help(help_command->parser);
                } else {
                    ap_print_help(parser);
                }
                ap_set_status(parser, AP_STATUS_HELP);
                return false;
            }
        }
        if (!ap_append_positional(parser, arg)) return false;
        if (parser->first_pos_arg_ends_option_parsing) options_enabled = false;
    }
    return ap_validate_positionals(parser);
}

bool ap_parse(ArgParser *parser, int argc, char **argv) {
    if (parser == NULL || argc < 0 || (argc > 0 && argv == NULL)) return false;
    if (ap_get_status(parser) != AP_STATUS_OK) return false;
    if (argc > 0) parser->zeroth_root_arg = argv[0];
    return ap_parse_level(parser, argc, argv, argc > 0 ? 1 : 0) && ap_get_status(parser) == AP_STATUS_OK;
}

ap_status_t ap_get_status(ArgParser *parser) {
    ArgParser *root = ap_root(parser);
    return root != NULL ? root->status : AP_STATUS_INVALID_ARGUMENT;
}

bool ap_had_memory_error(ArgParser *parser) { return ap_get_status(parser) == AP_STATUS_NO_MEMORY; }

char *ap_get_zeroth_root_arg(ArgParser *parser) {
    ArgParser *root = ap_root(parser);
    return root != NULL ? root->zeroth_root_arg : NULL;
}

void ap_print(ArgParser *parser) {
    if (parser == NULL) return;
    stdio__printf("Arguments:\n");
    for (int i = 0; i < parser->parsed_arg_count; ++i) stdio__printf("  %s\n", parser->parsed_args[i]);
    stdio__printf("Command:\n  %s\n", parser->cmd_name != NULL ? parser->cmd_name : "[none]");
}
