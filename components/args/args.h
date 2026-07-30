#pragma once

#include <stdbool.h>

typedef struct ArgParser ArgParser;
typedef int (*ap_callback_t)(char *cmd_name, ArgParser *cmd_parser);

typedef enum {
    AP_STATUS_OK = 0,
    AP_STATUS_HELP,
    AP_STATUS_VERSION,
    AP_STATUS_INVALID_ARGUMENT,
    AP_STATUS_NO_MEMORY,
} ap_status_t;

ArgParser *ap_new_parser(void);
void ap_free(ArgParser *parser);

void ap_set_helptext(ArgParser *parser, const char *helptext);
char *ap_get_helptext(ArgParser *parser);
void ap_set_version(ArgParser *parser, const char *version);
char *ap_get_version(ArgParser *parser);

/* Parses conventional main()-style arguments without modifying argv. Parse
 * errors, help, and version requests never exit the calling process. */
bool ap_parse(ArgParser *parser, int argc, char **argv);
ap_status_t ap_get_status(ArgParser *parser);
void ap_print_help(ArgParser *parser);

void ap_first_pos_arg_ends_option_parsing(ArgParser *parser);
void ap_all_args_as_pos_args(ArgParser *parser);
/* Accepts positional arguments beyond the declared named positionals. They
 * remain available through ap_get_arg_at_index()/ap_get_args(). */
void ap_allow_extra_args(ArgParser *parser);
/* Treats unregistered option-looking tokens as positional arguments while
 * continuing to recognize registered options. Useful for passwords, paths,
 * and other free-form positional values that may begin with '-'. */
void ap_unknown_options_as_args(ArgParser *parser);

void ap_add_flag(ArgParser *parser, const char *name);
void ap_add_str_opt(ArgParser *parser, const char *name, const char *fallback);
void ap_add_int_opt(ArgParser *parser, const char *name, int fallback);
void ap_add_dbl_opt(ArgParser *parser, const char *name, double fallback);
void ap_add_greedy_str_opt(ArgParser *parser, const char *name);

/* Adds text shown beside a previously registered option in generated help. */
void ap_set_opt_help(ArgParser *parser, const char *name, const char *helptext);

int ap_count(ArgParser *parser, const char *name);
bool ap_found(ArgParser *parser, const char *name);
char *ap_get_str_value(ArgParser *parser, const char *name);
char *ap_get_str_value_at_index(ArgParser *parser, const char *name, int index);
char **ap_get_str_values(ArgParser *parser, const char *name);
int ap_get_int_value(ArgParser *parser, const char *name);
int ap_get_int_value_at_index(ArgParser *parser, const char *name, int index);
int *ap_get_int_values(ArgParser *parser, const char *name);
double ap_get_dbl_value(ArgParser *parser, const char *name);
double ap_get_dbl_value_at_index(ArgParser *parser, const char *name, int index);
double *ap_get_dbl_values(ArgParser *parser, const char *name);

/* String results are borrowed and must not be modified or freed. Array
 * getters allocate only the returned array with memory__malloc(); release the
 * array with memory__free(), never libc free(). */

/* Named positionals are declared in order. Required arguments must precede
 * optional arguments. ap_get_arg() returns a borrowed argv string, or NULL
 * when an optional argument was omitted or the name is unknown. */
void ap_add_required_arg(ArgParser *parser, const char *name, const char *helptext);
void ap_add_optional_arg(ArgParser *parser, const char *name, const char *helptext);
char *ap_get_arg(ArgParser *parser, const char *name);

bool ap_has_args(ArgParser *parser);
int ap_count_args(ArgParser *parser);
char *ap_get_arg_at_index(ArgParser *parser, int index);
char **ap_get_args(ArgParser *parser);
int *ap_get_args_as_ints(ArgParser *parser);
double *ap_get_args_as_doubles(ArgParser *parser);

ArgParser *ap_new_cmd(ArgParser *parent_parser, const char *name);
void ap_set_cmd_callback(ArgParser *cmd_parser, ap_callback_t cmd_callback);
bool ap_found_cmd(ArgParser *parent_parser);
char *ap_get_cmd_name(ArgParser *parent_parser);
ArgParser *ap_get_cmd_parser(ArgParser *parent_parser);
int ap_get_cmd_exit_code(ArgParser *parent_parser);
void ap_enable_help_command(ArgParser *parent_parser, bool enable);
ArgParser *ap_get_parent(ArgParser *parser);

void ap_print(ArgParser *parser);
bool ap_had_memory_error(ArgParser *parser);
char *ap_get_zeroth_root_arg(ArgParser *parser);
