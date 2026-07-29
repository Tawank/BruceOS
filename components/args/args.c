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

static void ap_print_aliases(const char *names, const char *prefix) {
    const char *cursor = names;
    bool first = true;
    while (cursor != NULL && *cursor != '\0') {
        while (*cursor == ' ') cursor++;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ') cursor++;
        if (cursor == start) break;
        stdio__printf("%s%s%.*s", first ? "" : ", ", prefix, (int)(cursor - start), start);
        first = false;
    }
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
    if (parser->helptext != NULL && parser->helptext[0] != '\0') stdio__printf("\n%s\n", parser->helptext);

    if (parser->command_count > 0) {
        stdio__printf("\nCommands:\n");
        for (int i = 0; i < parser->command_count; ++i) {
            ap_command_t *command = &parser->commands[i];
            stdio__printf("  ");
            ap_print_aliases(command->names, "");
            for (int j = 0; j < command->parser->positional_count; ++j) {
                ap_positional_t *positional = &command->parser->positionals[j];
                stdio__printf(positional->required ? " <%s>" : " [%s]", positional->name);
            }
            if (command->parser->allow_extra_args) stdio__printf(" [args...]");
            if (command->parser->command_count > 0) stdio__printf(" <command>");
            if (command->parser->helptext != NULL) stdio__printf("\t%s", command->parser->helptext);
            stdio__printf("\n");
        }
    }

    if (parser->positional_count > 0) {
        stdio__printf("\nArguments:\n");
        for (int i = 0; i < parser->positional_count; ++i) {
            ap_positional_t *positional = &parser->positionals[i];
            stdio__printf("  %s%s", positional->name, positional->required ? " (required)" : "");
            if (positional->helptext != NULL) stdio__printf("\t%s", positional->helptext);
            stdio__printf("\n");
        }
    }

    stdio__printf("\nOptions:\n");
    for (int i = 0; i < parser->option_count; ++i) {
        ap_option_t *option = parser->options[i];
        stdio__printf("  ");
        const char *cursor = option->names;
        bool first = true;
        while (*cursor != '\0') {
            while (*cursor == ' ') cursor++;
            const char *start = cursor;
            while (*cursor != '\0' && *cursor != ' ') cursor++;
            if (cursor == start) break;
            stdio__printf("%s%s%.*s", first ? "" : ", ", cursor - start == 1 ? "-" : "--", (int)(cursor - start), start);
            first = false;
        }
        if (option->type != AP_OPT_FLAG) stdio__printf(" <value>");
        if (option->helptext != NULL) stdio__printf("\t%s", option->helptext);
        stdio__printf("\n");
    }
    stdio__printf("  -h, --help\tShow this help\n");
    if (parser->version != NULL) stdio__printf("  -v, --version\tShow version\n");
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
        if (options_enabled && (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0)) {
            ap_print_help(parser);
            ap_set_status(parser, AP_STATUS_HELP);
            return false;
        }
        if (options_enabled && parser->version != NULL && (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0)) {
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
