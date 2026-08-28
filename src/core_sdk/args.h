#pragma once

#include <stdbool.h>

/**
 * @brief Command-line argument parser (argparse-style).
 *
 * Generic option/flag/positional-argument and subcommand parser, independent
 * of the rest of core_sdk. Not permission-gated.
 * Docs: https://www.dmulholl.com/docs/args/master/
 */

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

/**
 * @brief Parses conventional main()-style arguments without modifying argv.
 *
 * Parse errors, help, and version requests never exit the calling process.
 *
 * @param parser Parser to run.
 * @param argc Argument count, as passed to main().
 * @param argv Argument vector, as passed to main(); not modified.
 */
bool ap_parse(ArgParser *parser, int argc, char **argv);

/**
 * @brief Outcome of the most recent ap_parse() call.
 *
 * @param parser Parser to query.
 */
ap_status_t ap_get_status(ArgParser *parser);

/**
 * @brief Prints the parser's generated help text.
 *
 * @param parser Parser to print help for.
 */
void ap_print_help(ArgParser *parser);

void ap_first_pos_arg_ends_option_parsing(ArgParser *parser);
void ap_all_args_as_pos_args(ArgParser *parser);

/**
 * @brief Accepts positional arguments beyond the declared named positionals.
 *
 * They remain available through ap_get_arg_at_index()/ap_get_args().
 *
 * @param parser Parser to configure.
 */
void ap_allow_extra_args(ArgParser *parser);

/**
 * @brief Treats unregistered option-looking tokens as positional arguments.
 *
 * Continues to recognize registered options.
 *
 * @param parser Parser to configure.
 */
void ap_unknown_options_as_args(ArgParser *parser);

void ap_add_flag(ArgParser *parser, const char *name);
void ap_add_str_opt(ArgParser *parser, const char *name, const char *fallback);
void ap_add_int_opt(ArgParser *parser, const char *name, int fallback);
void ap_add_dbl_opt(ArgParser *parser, const char *name, double fallback);
void ap_add_greedy_str_opt(ArgParser *parser, const char *name);
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

/**
 * @brief String results are borrowed and must not be modified or freed.
 *
 * Array getters allocate only the returned array with memory__malloc();
 * release the array with memory__free(), never libc free().
 */

/**
 * @brief Declares a required named positional argument.
 *
 * Named positionals are declared in order; required arguments must precede
 * optional arguments.
 *
 * @param parser Parser to configure.
 * @param name Name of the positional argument.
 * @param helptext Help text shown for this argument.
 */
void ap_add_required_arg(ArgParser *parser, const char *name, const char *helptext);

/**
 * @brief Declares an optional named positional argument.
 *
 * Named positionals are declared in order; required arguments must precede
 * optional arguments.
 *
 * @param parser Parser to configure.
 * @param name Name of the positional argument.
 * @param helptext Help text shown for this argument.
 */
void ap_add_optional_arg(ArgParser *parser, const char *name, const char *helptext);

/**
 * @brief Reads the value of a declared named positional argument.
 *
 * Returns a borrowed argv string, or NULL when an optional argument was
 * omitted or the name is unknown.
 *
 * @param parser Parser to query.
 * @param name Name of the positional argument, as declared.
 */
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
