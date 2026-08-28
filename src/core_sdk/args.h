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

/**
 * @brief Creates an empty argument parser.
 */
ArgParser *ap_new_parser(void);
/**
 * @brief Destroys an argument parser.
 *
 * @param parser Parser to destroy.
 */
void ap_free(ArgParser *parser);

/**
 * @brief Sets the program description shown in help output.
 *
 * @param parser Parser to configure.
 * @param helptext Program description to display.
 */
void ap_set_helptext(ArgParser *parser, const char *helptext);
/**
 * @brief Returns the program description shown in help output.
 *
 * @param parser Parser to query.
 */
char *ap_get_helptext(ArgParser *parser);
/**
 * @brief Sets the version text shown by the version option.
 *
 * @param parser Parser to configure.
 * @param version Version text to display.
 */
void ap_set_version(ArgParser *parser, const char *version);
/**
 * @brief Returns the configured version text.
 *
 * @param parser Parser to query.
 */
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

/**
 * @brief Stops parsing options after the first positional argument.
 *
 * @param parser Parser to configure.
 */
void ap_first_pos_arg_ends_option_parsing(ArgParser *parser);
/**
 * @brief Treats every argument as positional.
 *
 * @param parser Parser to configure.
 */
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

/**
 * @brief Adds a boolean command-line flag.
 *
 * @param parser Parser to configure.
 * @param name Flag name.
 */
void ap_add_flag(ArgParser *parser, const char *name);
/**
 * @brief Adds a string option.
 *
 * @param parser Parser to configure.
 * @param name Option name.
 * @param fallback Value used when the option is omitted.
 */
void ap_add_str_opt(ArgParser *parser, const char *name, const char *fallback);
/**
 * @brief Adds an integer option.
 *
 * @param parser Parser to configure.
 * @param name Option name.
 * @param fallback Value used when the option is omitted.
 */
void ap_add_int_opt(ArgParser *parser, const char *name, int fallback);
/**
 * @brief Adds a decimal-number option.
 *
 * @param parser Parser to configure.
 * @param name Option name.
 * @param fallback Value used when the option is omitted.
 */
void ap_add_dbl_opt(ArgParser *parser, const char *name, double fallback);
/**
 * @brief Adds a string option that accepts multiple values.
 *
 * @param parser Parser to configure.
 * @param name Option name.
 */
void ap_add_greedy_str_opt(ArgParser *parser, const char *name);
/**
 * @brief Sets the help text for an option or flag.
 *
 * @param parser Parser to configure.
 * @param name Option or flag name.
 * @param helptext Help text to display.
 */
void ap_set_opt_help(ArgParser *parser, const char *name, const char *helptext);

/**
 * @brief Returns how many values were provided for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
int ap_count(ArgParser *parser, const char *name);
/**
 * @brief Checks whether an option or flag was provided.
 *
 * @param parser Parser to query.
 * @param name Option or flag name.
 */
bool ap_found(ArgParser *parser, const char *name);
/**
 * @brief Returns the first string value for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
char *ap_get_str_value(ArgParser *parser, const char *name);
/**
 * @brief Returns one string value for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 * @param index Zero-based value index.
 */
char *ap_get_str_value_at_index(ArgParser *parser, const char *name, int index);
/**
 * @brief Returns all string values for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
char **ap_get_str_values(ArgParser *parser, const char *name);
/**
 * @brief Returns the first integer value for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
int ap_get_int_value(ArgParser *parser, const char *name);
/**
 * @brief Returns one integer value for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 * @param index Zero-based value index.
 */
int ap_get_int_value_at_index(ArgParser *parser, const char *name, int index);
/**
 * @brief Returns all integer values for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
int *ap_get_int_values(ArgParser *parser, const char *name);
/**
 * @brief Returns the first decimal value for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
double ap_get_dbl_value(ArgParser *parser, const char *name);
/**
 * @brief Returns one decimal value for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 * @param index Zero-based value index.
 */
double ap_get_dbl_value_at_index(ArgParser *parser, const char *name, int index);
/**
 * @brief Returns all decimal values for an option.
 *
 * @param parser Parser to query.
 * @param name Option name.
 */
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

/**
 * @brief Checks whether any positional arguments were provided.
 *
 * @param parser Parser to query.
 */
bool ap_has_args(ArgParser *parser);
/**
 * @brief Returns the number of positional arguments.
 *
 * @param parser Parser to query.
 */
int ap_count_args(ArgParser *parser);
/**
 * @brief Returns a positional argument by index.
 *
 * @param parser Parser to query.
 * @param index Zero-based argument index.
 */
char *ap_get_arg_at_index(ArgParser *parser, int index);

/**
 * @brief Returns all positional arguments as strings.
 *
 * @param parser Parser to query.
 */
char **ap_get_args(ArgParser *parser);
/**
 * @brief Returns all positional arguments as integers.
 *
 * @param parser Parser to query.
 */
int *ap_get_args_as_ints(ArgParser *parser);
/**
 * @brief Returns all positional arguments as decimal numbers.
 *
 * @param parser Parser to query.
 */
double *ap_get_args_as_doubles(ArgParser *parser);

/**
 * @brief Adds a subcommand and returns its parser.
 *
 * @param parent_parser Parent parser to configure.
 * @param name Subcommand name.
 */
ArgParser *ap_new_cmd(ArgParser *parent_parser, const char *name);
/**
 * @brief Sets the function called for a subcommand.
 *
 * @param cmd_parser Subcommand parser to configure.
 * @param cmd_callback Function to call after parsing the subcommand.
 */
void ap_set_cmd_callback(ArgParser *cmd_parser, ap_callback_t cmd_callback);
/**
 * @brief Checks whether a subcommand was provided.
 *
 * @param parent_parser Parent parser to query.
 */
bool ap_found_cmd(ArgParser *parent_parser);
/**
 * @brief Returns the selected subcommand name.
 *
 * @param parent_parser Parent parser to query.
 */
char *ap_get_cmd_name(ArgParser *parent_parser);
/**
 * @brief Returns the selected subcommand parser.
 *
 * @param parent_parser Parent parser to query.
 */
ArgParser *ap_get_cmd_parser(ArgParser *parent_parser);
/**
 * @brief Returns the selected subcommand callback result.
 *
 * @param parent_parser Parent parser to query.
 */
int ap_get_cmd_exit_code(ArgParser *parent_parser);
/**
 * @brief Enables or disables the built-in help subcommand.
 *
 * @param parent_parser Parent parser to configure.
 * @param enable True to enable the help subcommand.
 */
void ap_enable_help_command(ArgParser *parent_parser, bool enable);
/**
 * @brief Returns a subcommand parser’s parent.
 *
 * @param parser Subcommand parser to query.
 */
ArgParser *ap_get_parent(ArgParser *parser);

/**
 * @brief Prints the parser configuration for debugging.
 *
 * @param parser Parser to print.
 */
void ap_print(ArgParser *parser);
/**
 * @brief Checks whether parser setup ran out of memory.
 *
 * @param parser Parser to query.
 */
bool ap_had_memory_error(ArgParser *parser);
/**
 * @brief Returns the program name from the root argument list.
 *
 * @param parser Parser to query.
 */
char *ap_get_zeroth_root_arg(ArgParser *parser);
