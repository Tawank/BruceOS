#include "shell_compound.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "shell_arith.h"
#include "shell_builtins.h"
#include "shell_executor.h"
#include "shell_glob.h"
#include "shell_parser.h"

static void shell_compound__trim(const char *text, size_t length, size_t *out_start, size_t *out_len) {
    size_t start = 0;
    size_t end = length;
    while (start < end && isspace((unsigned char)text[start])) start++;
    while (end > start && isspace((unsigned char)text[end - 1])) end--;
    *out_start = start;
    *out_len = end - start;
}

/* Matches `keyword` ("if"/"elif"/"then"/"else"/"fi") against cmd's trimmed
 * text as a reserved word in command position: either the whole command is
 * bare (== keyword, nothing else -- the rest of the construct starts at the
 * next flat command) or it starts with `keyword` followed by whitespace,
 * with the remainder ("then echo hi", "if [ -n \"$x\" ]") returned as
 * *rest / *rest_len for the caller to run as the construct's first statement.
 * A word that merely starts with `keyword` (e.g. "ifconfig") never matches. */
static bool shell_compound__match_keyword(
    const shell_command_t *cmd, const char *keyword, const char **rest, size_t *rest_len
) {
    size_t start, len;
    shell_compound__trim(cmd->text, cmd->length, &start, &len);
    const char *text = cmd->text + start;
    size_t keyword_len = strlen(keyword);
    if (len < keyword_len || memcmp(text, keyword, keyword_len) != 0) return false;
    if (len == keyword_len) {
        *rest = text + keyword_len;
        *rest_len = 0;
        return true;
    }
    if (!isspace((unsigned char)text[keyword_len])) return false;
    size_t rs = keyword_len;
    size_t rl = len - keyword_len;
    while (rl > 0 && isspace((unsigned char)text[rs])) {
        rs++;
        rl--;
    }
    *rest = text + rs;
    *rest_len = rl;
    return true;
}

static bool shell_compound__is_close_brace(const shell_command_t *cmd) {
    size_t start, len;
    shell_compound__trim(cmd->text, cmd->length, &start, &len);
    return len == 1 && cmd->text[start] == '}';
}

/* Finds a "{" that stands as its own word in [text, text+length) -- preceded
 * and followed only by whitespace (or the ends of the range) -- skipping
 * over quoted/escaped content so a literal brace inside a string doesn't
 * false-positive. This is the only brace shell_compound.c ever looks for;
 * an unrelated "{" used as a plain argument (e.g. `echo { hi }`, where it's
 * the *second* word of a simple command, not the start of one) is filtered
 * out separately by shell_compound__parse_header() requiring a real
 * function-header shape before the brace. */
static bool shell_compound__find_open_brace(const char *text, size_t length, size_t *out_offset) {
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (single || double_quote) continue;
        if (c == '{' && (i == 0 || isspace((unsigned char)text[i - 1])) &&
            (i + 1 >= length || isspace((unsigned char)text[i + 1]))) {
            *out_offset = i;
            return true;
        }
    }
    return false;
}

/* Validates a function-definition header ("NAME", "NAME()", "NAME ( )",
 * "function NAME", "function NAME()") and extracts NAME. A bare NAME with no
 * "()" is only a function header when it's prefixed by "function" -- without
 * that keyword, "()" is mandatory, matching bash's grammar (otherwise a
 * stray "word {" like `echo { hi }`'s "echo" would look like a def). */
static bool shell_compound__parse_header(const char *raw, size_t raw_length, char *name_out, size_t name_cap) {
    size_t start, len;
    shell_compound__trim(raw, raw_length, &start, &len);
    const char *text = raw + start;

    static const char prefix[] = "function";
    size_t prefix_len = sizeof(prefix) - 1;
    bool has_prefix = false;
    if (len > prefix_len && memcmp(text, prefix, prefix_len) == 0 && isspace((unsigned char)text[prefix_len])) {
        size_t s2, l2;
        shell_compound__trim(text + prefix_len, len - prefix_len, &s2, &l2);
        text += prefix_len + s2;
        len = l2;
        has_prefix = true;
    }

    size_t name_len = 0;
    while (name_len < len && (isalnum((unsigned char)text[name_len]) || text[name_len] == '_')) name_len++;
    if (name_len == 0 || name_len >= name_cap || !shell_parser__valid_name(text, name_len)) return false;

    size_t rs = name_len;
    size_t rl = len - name_len;
    while (rl > 0 && isspace((unsigned char)text[rs])) {
        rs++;
        rl--;
    }
    bool has_parens = rl > 0 && text[rs] == '(';
    if (has_parens) {
        rs++;
        rl--;
        while (rl > 0 && isspace((unsigned char)text[rs])) {
            rs++;
            rl--;
        }
        if (rl == 0 || text[rs] != ')') return false;
        rs++;
        rl--;
        while (rl > 0 && isspace((unsigned char)text[rs])) {
            rs++;
            rl--;
        }
    }
    if (rl != 0 || (!has_prefix && !has_parens)) return false;

    memcpy(name_out, text, name_len);
    name_out[name_len] = '\0';
    return true;
}

/* Detects a function definition starting at plan->commands[index], in
 * either of its two source shapes -- "name() {" glued onto one flat command
 * (the common case: nothing but whitespace between the header and "{"), or
 * the header and a bare "{" as two separate flat commands (the brace on its
 * own line). Either way, *body_start / *body_end become a raw, verbatim slice
 * of the original source between the "{" and the matching bare "}" flat
 * command at *close_index -- re-parsed from scratch on every call via
 * shell_compound__run(), so it can itself contain if/fi, nested commands,
 * anything a top-level block can. Only the first bare "}" is matched (no
 * brace-depth counting), so a body containing its own nested "{ ... }" is
 * unsupported -- a deliberate simplification for this basic implementation. */
static bool shell_compound__match_function_header(
    const shell_plan_t *plan, size_t index, char *name_out, size_t name_cap, const char **body_start,
    const char **body_end, size_t *close_index
) {
    if (index >= plan->count) return false;
    const shell_command_t *cmd = &plan->commands[index];
    size_t brace_offset;
    const char *after_brace;
    size_t search_from;
    if (shell_compound__find_open_brace(cmd->text, cmd->length, &brace_offset)) {
        if (!shell_compound__parse_header(cmd->text, brace_offset, name_out, name_cap)) return false;
        after_brace = cmd->text + brace_offset + 1;
        search_from = index + 1;
    } else {
        if (!shell_compound__parse_header(cmd->text, cmd->length, name_out, name_cap)) return false;
        if (index + 1 >= plan->count) return false;
        const shell_command_t *brace_cmd = &plan->commands[index + 1];
        size_t s, l;
        shell_compound__trim(brace_cmd->text, brace_cmd->length, &s, &l);
        if (l != 1 || brace_cmd->text[s] != '{') return false;
        after_brace = brace_cmd->text + brace_cmd->length;
        search_from = index + 2;
    }
    for (size_t k = search_from; k < plan->count; ++k) {
        if (shell_compound__is_close_brace(&plan->commands[k])) {
            *body_start = after_brace;
            *body_end = plan->commands[k].text;
            *close_index = k;
            return true;
        }
    }
    return false;
}

typedef enum {
    SHELL_COMPOUND_PLAIN,
    SHELL_COMPOUND_IF,
    SHELL_COMPOUND_ELIF,
    SHELL_COMPOUND_THEN,
    SHELL_COMPOUND_ELSE,
    SHELL_COMPOUND_FI,
    SHELL_COMPOUND_CLOSE_BRACE,
    SHELL_COMPOUND_FUNCTION,
    SHELL_COMPOUND_FOR,
    SHELL_COMPOUND_WHILE,
    SHELL_COMPOUND_DO,
    SHELL_COMPOUND_DONE,
    SHELL_COMPOUND_CASE,
    SHELL_COMPOUND_ESAC,
} shell_compound_kind_t;

static shell_compound_kind_t shell_compound__classify(const shell_plan_t *plan, size_t index) {
    const shell_command_t *cmd = &plan->commands[index];
    const char *rest;
    size_t rest_len;
    if (shell_compound__is_close_brace(cmd)) return SHELL_COMPOUND_CLOSE_BRACE;
    if (shell_compound__match_keyword(cmd, "if", &rest, &rest_len)) return SHELL_COMPOUND_IF;
    if (shell_compound__match_keyword(cmd, "elif", &rest, &rest_len)) return SHELL_COMPOUND_ELIF;
    if (shell_compound__match_keyword(cmd, "then", &rest, &rest_len)) return SHELL_COMPOUND_THEN;
    if (shell_compound__match_keyword(cmd, "else", &rest, &rest_len)) return SHELL_COMPOUND_ELSE;
    if (shell_compound__match_keyword(cmd, "fi", &rest, &rest_len)) return SHELL_COMPOUND_FI;
    if (shell_compound__match_keyword(cmd, "for", &rest, &rest_len)) return SHELL_COMPOUND_FOR;
    if (shell_compound__match_keyword(cmd, "while", &rest, &rest_len)) return SHELL_COMPOUND_WHILE;
    if (shell_compound__match_keyword(cmd, "do", &rest, &rest_len)) return SHELL_COMPOUND_DO;
    if (shell_compound__match_keyword(cmd, "done", &rest, &rest_len)) return SHELL_COMPOUND_DONE;
    if (shell_compound__match_keyword(cmd, "case", &rest, &rest_len)) return SHELL_COMPOUND_CASE;
    if (shell_compound__match_keyword(cmd, "esac", &rest, &rest_len)) return SHELL_COMPOUND_ESAC;
    char name[SHELL__FUNCTION_NAME_MAX];
    const char *body_start, *body_end;
    size_t close_index;
    if (shell_compound__match_function_header(plan, index, name, sizeof(name), &body_start, &body_end, &close_index)) {
        return SHELL_COMPOUND_FUNCTION;
    }
    return SHELL_COMPOUND_PLAIN;
}

/* Runs a single command text (an "if"/"elif"/"then"/"else" keyword's glued
 * remainder) as its own one-command plan. */
static int shell_compound__run_text(shell_state_t *state, const char *text, size_t length) {
    shell_command_t synthetic = {.text = text, .length = length, .connector = SHELL_CONNECT_NONE};
    shell_plan_t sub = {.commands = &synthetic, .count = 1};
    return shell_executor__plan(state, &sub);
}

static int shell_compound__define_function(shell_state_t *state, const char *name, const char *body_text, size_t body_len);

static int shell_compound__run_sequence(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute);
static int shell_compound__run_for(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute);
static int shell_compound__run_while(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute);
static int shell_compound__run_case(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute);

/* A run_sequence() pass that was actually executing can stop early, part
 * way through the statements it was walking, because a "break" or "exit"
 * fired inside them (see shell_state_t.break_requested/exit_requested) --
 * leaving *index short of the then/elif/else/fi/do/done boundary it would
 * otherwise have reached. Callers that go on to inspect *index's classify()
 * right after such a call (run_if's then/fi checks below, chiefly) call
 * this first: a non-executing pass can never itself stop early (nothing it
 * does can set either flag), so it always finishes walking to the real
 * boundary, without re-running anything or emitting a second, spurious
 * "missing ..." error over the interrupted one. */
static void shell_compound__catch_up(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool was_executing) {
    if (was_executing && (state->exit_requested || state->break_requested > 0)) {
        (void)shell_compound__run_sequence(state, plan, index, false);
    }
}

/* *index is at the "if" (or, on a later loop iteration, "elif") command on
 * entry. Consumes through the matching "fi" and leaves *index just past it.
 * `execute` false means this whole if-statement lives in a branch that
 * isn't being taken (an outer if/function skip) -- conditions are never run
 * and no branch is ever taken, but the structure is still walked so *index
 * ends up in the right place. */
static int shell_compound__run_if(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute) {
    bool taken = false;
    int status = 0;
    bool first = true;
    for (;;) {
        const shell_command_t *cmd = &plan->commands[*index];
        const char *rest;
        size_t rest_len;
        (void)shell_compound__match_keyword(cmd, first ? "if" : "elif", &rest, &rest_len);
        first = false;
        (*index)++;

        bool cond_execute = execute && !taken;
        bool have_cond = rest_len > 0;
        int cond_status = have_cond && cond_execute ? shell_compound__run_text(state, rest, rest_len) : 1;
        /* The rest of the condition list (if any) is itself just a run of
         * statements -- reuse run_sequence rather than re-deriving its
         * then/elif/else/fi/"}" boundary handling here. */
        if (*index < plan->count && shell_compound__classify(plan, *index) != SHELL_COMPOUND_THEN) {
            int seq_status = shell_compound__run_sequence(state, plan, index, cond_execute);
            shell_compound__catch_up(state, plan, index, cond_execute);
            if (cond_execute) cond_status = seq_status;
            have_cond = true;
        }
        if (*index >= plan->count || shell_compound__classify(plan, *index) != SHELL_COMPOUND_THEN) {
            stdio__printf("shell: if: missing 'then'\n");
            state->last_status = 2;
            *index = plan->count; /* stop the enclosing walk cleanly instead of resuming mid-error */
            return 2;
        }
        cmd = &plan->commands[*index];
        (void)shell_compound__match_keyword(cmd, "then", &rest, &rest_len);
        (*index)++;

        bool run_branch = cond_execute && have_cond && cond_status == 0;
        if (run_branch) taken = true;
        if (rest_len > 0 && run_branch) status = shell_compound__run_text(state, rest, rest_len);
        int body_status = shell_compound__run_sequence(state, plan, index, run_branch);
        shell_compound__catch_up(state, plan, index, run_branch);
        if (run_branch) status = body_status;

        if (*index >= plan->count) {
            stdio__printf("shell: if: missing 'fi'\n");
            state->last_status = 2;
            return 2;
        }
        if (shell_compound__classify(plan, *index) == SHELL_COMPOUND_ELIF) continue;
        break;
    }

    if (*index < plan->count && shell_compound__classify(plan, *index) == SHELL_COMPOUND_ELSE) {
        const shell_command_t *cmd = &plan->commands[*index];
        const char *rest;
        size_t rest_len;
        (void)shell_compound__match_keyword(cmd, "else", &rest, &rest_len);
        (*index)++;
        bool run_branch = execute && !taken;
        if (run_branch) taken = true;
        if (rest_len > 0 && run_branch) status = shell_compound__run_text(state, rest, rest_len);
        int body_status = shell_compound__run_sequence(state, plan, index, run_branch);
        shell_compound__catch_up(state, plan, index, run_branch);
        if (run_branch) status = body_status;
    }

    if (*index >= plan->count || shell_compound__classify(plan, *index) != SHELL_COMPOUND_FI) {
        stdio__printf("shell: if: missing 'fi'\n");
        state->last_status = 2;
        *index = plan->count; /* stop the enclosing walk cleanly instead of resuming mid-error */
        return 2;
    }
    (*index)++;

    if (execute && !taken) status = 0; /* bash: no branch taken and no else -> exit status 0 */
    return status;
}

/* Walks plan->commands from *index, running plain commands (grouped into
 * runs so ;/&&/|| short-circuiting between them still works, via
 * shell_executor__plan) and recursing into nested "if"/function-definition
 * constructs, until it either runs out of commands or reaches a
 * then/elif/else/fi/"}" that belongs to an *enclosing* construct -- which it
 * leaves at *index, unconsumed, for that caller to see. `execute` false
 * skips all side effects (no command runs, no function gets defined) while
 * still advancing *index the same amount, for an if-branch that isn't
 * taken. */
static int shell_compound__run_sequence(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute) {
    int status = 0;
    while (*index < plan->count && !(execute && (state->exit_requested || state->break_requested > 0))) {
        /* A ";;"-tagged entry belongs to an *enclosing* case's next clause
         * (or is "esac" itself), never to whatever body is currently being
         * walked -- checked ahead of classify() since the entry's own text
         * (e.g. a bare pattern like "b)") classifies as perfectly ordinary
         * SHELL_COMPOUND_PLAIN otherwise. See SHELL_CONNECT_CASE_END's own
         * doc comment in shell_parser.h. */
        if (plan->commands[*index].connector == SHELL_CONNECT_CASE_END) break;
        shell_compound_kind_t kind = shell_compound__classify(plan, *index);
        if (kind == SHELL_COMPOUND_THEN || kind == SHELL_COMPOUND_ELIF || kind == SHELL_COMPOUND_ELSE ||
            kind == SHELL_COMPOUND_FI || kind == SHELL_COMPOUND_CLOSE_BRACE || kind == SHELL_COMPOUND_DO ||
            kind == SHELL_COMPOUND_DONE || kind == SHELL_COMPOUND_ESAC) {
            break;
        }
        if (kind == SHELL_COMPOUND_IF) {
            status = shell_compound__run_if(state, plan, index, execute);
            continue;
        }
        if (kind == SHELL_COMPOUND_FOR) {
            status = shell_compound__run_for(state, plan, index, execute);
            continue;
        }
        if (kind == SHELL_COMPOUND_WHILE) {
            status = shell_compound__run_while(state, plan, index, execute);
            continue;
        }
        if (kind == SHELL_COMPOUND_CASE) {
            status = shell_compound__run_case(state, plan, index, execute);
            continue;
        }
        if (kind == SHELL_COMPOUND_FUNCTION) {
            char name[SHELL__FUNCTION_NAME_MAX];
            const char *body_start, *body_end;
            size_t close_index;
            (void)shell_compound__match_function_header(
                plan, *index, name, sizeof(name), &body_start, &body_end, &close_index
            );
            if (execute) {
                status = shell_compound__define_function(state, name, body_start, (size_t)(body_end - body_start));
            }
            *index = close_index + 1;
            continue;
        }
        size_t run_start = *index;
        (*index)++;
        while (*index < plan->count && plan->commands[*index].connector != SHELL_CONNECT_CASE_END &&
               shell_compound__classify(plan, *index) == SHELL_COMPOUND_PLAIN) {
            (*index)++;
        }
        if (execute) {
            shell_plan_t sub = {.commands = &plan->commands[run_start], .count = *index - run_start};
            status = shell_executor__plan(state, &sub);
        }
    }
    return status;
}

/* A parsed "for" header: either bash's C-style `((init; cond; incr))` (see
 * shell_arith.c), or its word-list form `NAME [in WORD...]` -- a bare NAME
 * with no "in" clause iterates $1.. of the *currently executing function
 * call* (state->positional), same as bash's own fallback to "$@"; at top
 * level, outside any call, that's simply empty, so the loop runs zero
 * times. */
typedef struct {
    bool c_style;
    const char *init_text, *cond_text, *incr_text;
    size_t init_len, cond_len, incr_len;
    char name[SHELL__VARIABLE_NAME_MAX];
    bool has_in;
    const char *list_text;
    size_t list_len;
} shell_compound__for_header_t;

/* Splits `inner` (the text strictly between a C-style header's "((" and
 * "))") into exactly 3 segments on ';' -- the init/cond/incr clauses --
 * ignoring any ';' nested inside a parenthesized sub-group. Each segment may
 * be empty (`for ((;;))`'s infinite-loop shape). Returns false if `inner`
 * doesn't contain exactly 2 top-level ';'. */
static bool shell_compound__split_arith_header(
    const char *inner, size_t inner_len, const char *out_text[3], size_t out_len[3]
) {
    size_t segment_start = 0;
    int segment = 0;
    int depth = 0;
    for (size_t i = 0; i <= inner_len; ++i) {
        bool at_end = i == inner_len;
        char c = at_end ? ';' : inner[i];
        if (!at_end) {
            if (c == '(') depth++;
            else if (c == ')') depth--;
        }
        if (at_end || (depth == 0 && c == ';')) {
            if (segment >= 3) return false;
            out_text[segment] = inner + segment_start;
            out_len[segment] = i - segment_start;
            segment++;
            segment_start = i + 1;
        }
    }
    return segment == 3;
}

static bool
shell_compound__parse_for_header(const char *raw, size_t raw_length, shell_compound__for_header_t *out) {
    size_t start, len;
    shell_compound__trim(raw, raw_length, &start, &len);
    const char *text = raw + start;
    if (len == 0) return false;

    if (len >= 4 && text[0] == '(' && text[1] == '(' && text[len - 1] == ')' && text[len - 2] == ')') {
        const char *parts[3];
        size_t parts_len[3];
        if (!shell_compound__split_arith_header(text + 2, len - 4, parts, parts_len)) return false;
        out->c_style = true;
        out->init_text = parts[0];
        out->init_len = parts_len[0];
        out->cond_text = parts[1];
        out->cond_len = parts_len[1];
        out->incr_text = parts[2];
        out->incr_len = parts_len[2];
        return true;
    }

    size_t name_len = 0;
    while (name_len < len && (isalnum((unsigned char)text[name_len]) || text[name_len] == '_')) name_len++;
    if (name_len == 0 || name_len >= sizeof(out->name) || !shell_parser__valid_name(text, name_len)) return false;
    out->c_style = false;
    memcpy(out->name, text, name_len);
    out->name[name_len] = '\0';

    size_t rs = name_len, rl = len - name_len;
    while (rl > 0 && isspace((unsigned char)text[rs])) {
        rs++;
        rl--;
    }
    if (rl == 0) {
        out->has_in = false;
        out->list_text = NULL;
        out->list_len = 0;
        return true;
    }
    if (rl < 2 || memcmp(text + rs, "in", 2) != 0 || (rl > 2 && !isspace((unsigned char)text[rs + 2]))) {
        return false; /* trailing garbage after NAME that isn't a real "in ..." clause */
    }
    rs += 2;
    rl -= 2;
    while (rl > 0 && isspace((unsigned char)text[rs])) {
        rs++;
        rl--;
    }
    out->has_in = true;
    out->list_text = text + rs;
    out->list_len = rl;
    return true;
}

/* Shared by run_for()/run_while(): consumes plan->commands from *index
 * (already positioned right after the "for"/"while" header, or mid-way
 * through an unglued condition list) forward to the next SHELL_COMPOUND_DO,
 * via a non-executing dry pass when there's more than the header's own
 * glued remainder to walk through. Prints/records the "missing 'do'" error
 * itself on failure. Returns false on failure, having already set *index to
 * plan->count. */
static bool shell_compound__find_do(shell_state_t *state, const shell_plan_t *plan, size_t *index, const char *keyword) {
    if (*index < plan->count && shell_compound__classify(plan, *index) != SHELL_COMPOUND_DO) {
        (void)shell_compound__run_sequence(state, plan, index, false);
    }
    if (*index >= plan->count || shell_compound__classify(plan, *index) != SHELL_COMPOUND_DO) {
        stdio__printf("shell: %s: missing 'do'\n", keyword);
        state->last_status = 2;
        *index = plan->count;
        return false;
    }
    return true;
}

/* Same idea as shell_compound__find_do(), but for the matching "done" that
 * closes a loop body -- called with *index already past "do". */
static bool
shell_compound__find_done(shell_state_t *state, const shell_plan_t *plan, size_t *index, const char *keyword) {
    if (*index < plan->count && shell_compound__classify(plan, *index) != SHELL_COMPOUND_DONE) {
        (void)shell_compound__run_sequence(state, plan, index, false);
    }
    if (*index >= plan->count || shell_compound__classify(plan, *index) != SHELL_COMPOUND_DONE) {
        stdio__printf("shell: %s: missing 'done'\n", keyword);
        state->last_status = 2;
        *index = plan->count;
        return false;
    }
    return true;
}

/* Runs one loop-body iteration starting at `body_start` (a copy of it --
 * the loop's own *index into `plan` was already finalized past "done" by
 * the boundary-discovery pass before any of this runs, so this cursor is
 * disposable) up to `body_end` (exclusive, the "done" flat command's
 * index). Returns the branch's exit status.
 *
 * The "do"-glued first statement (see shell_compound__consume_do() below)
 * is *not* special-cased here -- by the time this runs, it has already been
 * folded into plan->commands[body_start] itself, so it's just the body's
 * first entry, walked -- and, if it's a nested if/for/while, correctly
 * recursed into -- like any other. */
static int shell_compound__run_loop_body(shell_state_t *state, const shell_plan_t *plan, size_t body_start, size_t body_end) {
    if (body_start >= body_end) return 0;
    size_t cursor = body_start;
    return shell_compound__run_sequence(state, plan, &cursor, true);
}

/* The plan entry shell_compound__consume_do() shrank, and what it originally
 * held -- see shell_compound__restore_do() below. `connector` is saved/
 * restored the same way (consume_do() itself never changes it -- restoring
 * is then a no-op there -- but shell_compound__run_case() below, which
 * builds this struct directly rather than through consume_do(), *does*
 * need to change it: see its own comment on why). */
typedef struct {
    size_t index;
    const char *text;
    size_t length;
    shell_connector_t connector;
} shell_compound__do_span_t;

/* *index is at the "do" that shell_compound__find_do() just confirmed is
 * there. Consumes it, leaving *index at the loop body's first entry, and
 * returns what plan->commands[] held there before -- pass this to
 * shell_compound__restore_do() once the caller is done needing the shrunk
 * form (see below for why that matters).
 *
 * shell_parser__plan() splits purely on ;/&&/||/pipe/newline, with no
 * keyword awareness -- so "do"'s glued remainder (e.g. "if $n -eq 3" in
 * "do if [ $n -eq 3 ]; then break; fi") can itself be the opening of a
 * nested if/for/while construct spanning several more plan entries, not
 * just a flat first statement the way "do echo hi" is. Running that
 * remainder as one flat command via shell_compound__run_text() (as this
 * used to) mis-executes it and, worse, desyncs the boundary-discovery dry
 * run that locates "done" for it, since a dry run never reaches this
 * function at all -- it only ever walks plan->commands[] via classify().
 *
 * So instead of hand-running the remainder, shrink this plan entry in
 * place down to just that remainder (dropping the "do " prefix) and leave
 * *index pointing at it, unconsumed: reclassify()'d normally, it now reads
 * as SHELL_COMPOUND_IF/FOR/WHILE (recursing exactly like a standalone
 * nested construct would) or plain SHELL_COMPOUND_PLAIN (an ordinary
 * command, run the same as before) -- either way it becomes an ordinary
 * first entry of the body for every walk (discovery and execution alike)
 * from here on, not a one-off special case. A "do" with nothing glued to
 * it (its own segment, e.g. "do" on its own line before a real body
 * statement) has nothing to shrink; *index just advances past it, and the
 * returned span is a no-op to restore. */
static shell_compound__do_span_t shell_compound__consume_do(const shell_plan_t *plan, size_t *index) {
    shell_compound__do_span_t saved = {
        .index = *index,
        .text = plan->commands[*index].text,
        .length = plan->commands[*index].length,
        .connector = plan->commands[*index].connector,
    };
    const char *head_text;
    size_t head_len;
    (void)shell_compound__match_keyword(&plan->commands[*index], "do", &head_text, &head_len);
    if (head_len > 0) {
        plan->commands[*index].text = head_text;
        plan->commands[*index].length = head_len;
    } else {
        (*index)++;
    }
    return saved;
}

/* Undoes shell_compound__consume_do()'s shrink. A nested for/while's "do"
 * entry gets shrunk once by whichever pass reaches it first -- often a
 * boundary-discovery dry run belonging to an *outer* loop, hunting for its
 * own "done" -- but that same nested construct is then run for real again on
 * every iteration of that outer loop, via a fresh, independent
 * run_for()/run_while() call each time (its own *index into plan is a local
 * copy, disposable per shell_compound__run_loop_body()'s own header comment
 * above). Each such call redoes its own find_do()/consume_do() from
 * scratch, expecting to find an unshrunk "do ..." entry to match against --
 * so a shrink left in place past the call that produced it desyncs every
 * later call onto the same entry (find_do() no longer sees SHELL_COMPOUND_DO
 * there at all). Restoring here, right before every return out of
 * run_for()/run_while() from this point on, keeps each call self-contained:
 * whatever shrink it performed to run its own body is gone again by the time
 * it hands control back, so the next call -- next iteration of an enclosing
 * loop, or a plain second look at the same plan -- starts from the same
 * pristine "do ..." text this one did. */
static void shell_compound__restore_do(const shell_plan_t *plan, const shell_compound__do_span_t *saved) {
    plan->commands[saved->index].text = saved->text;
    plan->commands[saved->index].length = saved->length;
    plan->commands[saved->index].connector = saved->connector;
}

/* True if this iteration should stop the loop: a break (consuming one
 * level) or an exit/kill signal (translated into exit_requested, same as
 * shell_app.c's own interactive-read cancellation handling) fired during
 * the iteration just run. */
static bool shell_compound__loop_should_stop(shell_state_t *state) {
    if (state->break_requested > 0) {
        state->break_requested--;
        return true;
    }
    if (state->exit_requested) return true;
    bruce_process_signal_t signal = process__current_signal();
    if (signal != 0) {
        state->exit_requested = true;
        state->exit_status = 128 + (int)signal;
        return true;
    }
    (void)runtime__delay(0); /* cooperative yield -- see the .c file's header comment on busy for/while bodies */
    return false;
}

static int shell_compound__run_for(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute) {
    const char *rest;
    size_t rest_len;
    (void)shell_compound__match_keyword(&plan->commands[*index], "for", &rest, &rest_len);
    (*index)++;

    shell_compound__for_header_t header;
    if (!shell_compound__parse_for_header(rest, rest_len, &header)) {
        stdio__printf("shell: for: malformed header\n");
        state->last_status = 2;
        *index = plan->count;
        return 2;
    }
    if (!shell_compound__find_do(state, plan, index, "for")) return 2;
    /* From here on, plan->commands[do_span.index] is shrunk to "do"'s glued
     * remainder -- every return below must go through "done" so it gets
     * restored first (see shell_compound__restore_do()'s header comment). */
    shell_compound__do_span_t do_span = shell_compound__consume_do(plan, index);
    size_t body_start = *index;
    bool have_done = shell_compound__find_done(state, plan, index, "for");
    size_t body_end = *index;
    if (have_done) (*index)++;

    int result;
    if (!have_done) {
        result = 2;
    } else if (!execute) {
        result = 0;
    } else {
        int status = 0;
        const char *error = NULL;
        bool malformed = false;
        if (header.c_style) {
            size_t s, l;
            long scratch;
            shell_compound__trim(header.init_text, header.init_len, &s, &l);
            if (l > 0 && !shell_arith__eval(state, header.init_text + s, l, &scratch, &error)) {
                stdio__printf("shell: for: %s\n", error != NULL ? error : "syntax error");
                state->last_status = 2;
                malformed = true;
            }
            while (!malformed) {
                shell_compound__trim(header.cond_text, header.cond_len, &s, &l);
                long cond_value = 1; /* an empty condition clause is always true, matching C's `for(;;)` */
                if (l > 0 && !shell_arith__eval(state, header.cond_text + s, l, &cond_value, &error)) {
                    stdio__printf("shell: for: %s\n", error != NULL ? error : "syntax error");
                    status = 2;
                    break;
                }
                if (cond_value == 0) break;

                status = shell_compound__run_loop_body(state, plan, body_start, body_end);
                if (shell_compound__loop_should_stop(state)) break;

                shell_compound__trim(header.incr_text, header.incr_len, &s, &l);
                if (l > 0 && !shell_arith__eval(state, header.incr_text + s, l, &scratch, &error)) {
                    stdio__printf("shell: for: %s\n", error != NULL ? error : "syntax error");
                    status = 2;
                    break;
                }
            }
        } else {
            char **words = NULL;
            int word_count = 0;
            if (header.has_in) {
                shell_command_t synthetic = {.text = header.list_text, .length = header.list_len, .connector = SHELL_CONNECT_NONE};
                const char *words_error = NULL;
                if (shell_parser__words(
                        &synthetic, &words, &word_count, shell_executor__lookup, shell_executor__run_substitution,
                        shell_executor__eval_arith_word, state, state->last_status, &words_error
                    ) != 0) {
                    stdio__printf("shell: for: %s\n", words_error != NULL ? words_error : "expansion error");
                    state->last_status = 2;
                    malformed = true;
                }
            }
            if (!malformed) {
                int count = header.has_in ? word_count : state->positional_count;
                for (int i = 0; i < count; ++i) {
                    const char *value = header.has_in ? words[i] : state->positional[i];
                    int assigned = shell_builtins__set(state, header.name, value);
                    if (assigned != 0) {
                        status = assigned;
                        break;
                    }
                    status = shell_compound__run_loop_body(state, plan, body_start, body_end);
                    if (shell_compound__loop_should_stop(state)) break;
                }
            }
            shell_parser__free_words(words, word_count);
        }
        result = malformed ? 2 : status;
    }
    shell_compound__restore_do(plan, &do_span);
    return result;
}

static int shell_compound__run_while(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute) {
    const char *cond_head;
    size_t cond_head_len;
    (void)shell_compound__match_keyword(&plan->commands[*index], "while", &cond_head, &cond_head_len);
    (*index)++;
    size_t cond_start = *index;
    if (!shell_compound__find_do(state, plan, index, "while")) return 2;
    size_t cond_end = *index;
    /* From here on, plan->commands[do_span.index] is shrunk to "do"'s glued
     * remainder -- every return below must go through "done" so it gets
     * restored first (see shell_compound__restore_do()'s header comment). */
    shell_compound__do_span_t do_span = shell_compound__consume_do(plan, index);
    size_t body_start = *index;
    bool have_done = shell_compound__find_done(state, plan, index, "while");
    size_t body_end = *index;
    if (have_done) (*index)++;

    int result;
    if (!have_done) {
        result = 2;
    } else if (!execute) {
        result = 0;
    } else {
        int status = 0;
        for (;;) {
            int cond_status = 0;
            if (cond_head_len > 0) cond_status = shell_compound__run_text(state, cond_head, cond_head_len);
            if (cond_start < cond_end) {
                size_t cursor = cond_start;
                cond_status = shell_compound__run_sequence(state, plan, &cursor, true);
            }
            if (shell_compound__loop_should_stop(state)) break;
            if (cond_status != 0) break;

            status = shell_compound__run_loop_body(state, plan, body_start, body_end);
            if (shell_compound__loop_should_stop(state)) break;
        }
        result = status;
    }
    shell_compound__restore_do(plan, &do_span);
    return result;
}

/* `case`'s glob matching (`*`/`?`/`[...]`/`[!...]`/`[^...]`) is
 * shell_glob__match() (shell_glob.c), shared with shell_parser.c's real
 * pathname expansion -- see shell_glob.h's own doc comment for exactly what
 * it supports. Unlike that pathname expansion, this never touches the
 * filesystem: it only ever compares a pattern against the one `text` string
 * shell_compound__run_case() already expanded, exactly the same job
 * `[[ str == pattern ]]` would do if this shell's `[[` implemented pattern
 * matching (shell_condition.c's own doc comment marks that as out of scope
 * there; this is `case`'s own independent use of the same glob syntax). */

/* Finds a standalone "in" token in [text, text+length) -- bounded by
 * whitespace/start/end on both sides, skipping quoted content the same way
 * shell_compound__find_open_brace() does (not command-substitution spans --
 * the same accepted simplification noted on that function; a case word
 * containing a literal "in" inside its own "$(...)" is the one input this
 * misreads). Splits a "case" header's remainder into its WORD (before "in")
 * and the first clause's glued text, if any (after "in") -- the case-word
 * equivalent of shell_compound__parse_for_header()'s own "in" search, except
 * the word here can itself contain whitespace (`case "$a $b" in`), so this
 * scans the whole remainder instead of assuming "in" immediately follows one
 * bare NAME. */
static bool shell_compound__find_case_in(const char *text, size_t length, size_t *out_in_start) {
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (single || double_quote) continue;
        if (c == 'i' && i + 1 < length && text[i + 1] == 'n' && (i == 0 || isspace((unsigned char)text[i - 1])) &&
            (i + 2 >= length || isspace((unsigned char)text[i + 2]))) {
            *out_in_start = i;
            return true;
        }
    }
    return false;
}

#define SHELL__MAX_CASE_PATTERNS 8

/* Finds the first top-level (quote/escape-aware, same limitation as
 * shell_compound__find_case_in() above re: command-substitution spans) ')'
 * in [text, text+length). Returns false -- nothing else set -- when there
 * isn't one. */
static bool shell_compound__find_case_close(const char *text, size_t length, size_t *out_offset) {
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            continue;
        }
        if (single || double_quote) continue;
        if (c == ')') {
            *out_offset = i;
            return true;
        }
    }
    return false;
}

/* Collects one clause's pattern list -- "PATTERN[|PATTERN...])" -- starting
 * at plan->commands[*index] (an optional leading '(' before the first
 * pattern, bash's "(pattern) cmd ;;" form, is skipped there), and reports
 * where its body begins.
 *
 * shell_parser__plan() has no notion of `case`, so a top-level '|' is
 * always tokenized as an ordinary pipe connector (SHELL_CONNECT_PIPE) --
 * splitting what bash reads as one "a|b|c)" pattern list across several
 * plan entries the exact same way "cmd1 | cmd2" would be (see
 * SHELL_CONNECT_PIPE's own doc comment in shell_parser.h). So rather than
 * scanning one entry's text for '|' -- which, by the time this runs, has
 * already been consumed as an entry boundary and never appears in any
 * entry's own text -- this walks *forward* through however many
 * PIPE-connected entries the list actually spans, each contributing
 * exactly one more pattern, until it finds one containing a top-level ')':
 * that entry's text up to the ')' is the list's last pattern, and *index is
 * left pointing at it (unshrunk) so shell_compound__run_case() can shrink
 * it down to its own glued body exactly the way it already does for a
 * single-entry clause. Every pattern-only entry consumed along the way is
 * left completely untouched (text/length/connector unchanged) -- unlike the
 * final, ')'-bearing entry, it never needs shrinking or restoring, since it
 * carries nothing else worth keeping and (for a clause inside a loop body
 * re-walked on every iteration) is simply re-read the same way again next
 * time. A ')'-less entry that ISN'T itself followed by a PIPE-connected one
 * means the list never closes -- reported by the caller as a malformed
 * clause. */
static bool shell_compound__collect_case_patterns(
    const shell_plan_t *plan, size_t *index, shell_word_span_t out_patterns[SHELL__MAX_CASE_PATTERNS],
    size_t *out_pattern_count, size_t *out_body_offset
) {
    size_t count = 0;
    bool first = true;
    for (;;) {
        const char *text = plan->commands[*index].text;
        size_t length = plan->commands[*index].length;
        size_t start, len;
        shell_compound__trim(text, length, &start, &len);
        if (first && len > 0 && text[start] == '(') {
            start++;
            len--;
        }
        first = false;
        size_t close_offset;
        if (shell_compound__find_case_close(text + start, len, &close_offset)) {
            size_t s = start, e = start + close_offset;
            while (s < e && isspace((unsigned char)text[s])) s++;
            while (e > s && isspace((unsigned char)text[e - 1])) e--;
            if (count >= SHELL__MAX_CASE_PATTERNS) return false;
            out_patterns[count].text = text + s;
            out_patterns[count].length = e - s;
            count++;
            *out_pattern_count = count;
            *out_body_offset = start + close_offset + 1;
            return true;
        }
        if (count >= SHELL__MAX_CASE_PATTERNS) return false;
        out_patterns[count].text = text + start;
        out_patterns[count].length = len;
        count++;
        if (*index + 1 >= plan->count || plan->commands[*index + 1].connector != SHELL_CONNECT_PIPE) return false;
        (*index)++;
    }
}

/* *index is at the "case" (or, on a nested call, a recursively-reached)
 * command on entry. Consumes through the matching "esac" and leaves *index
 * just past it, same contract as shell_compound__run_if()/run_for()/
 * run_while(). `execute` false walks the whole structure -- every clause,
 * every glob comparison skipped -- without ever expanding the case word or
 * a pattern (both can run "$(...)" command substitutions bash would never
 * run for a branch that isn't taken) or running any clause's body, purely
 * to leave *index in the right place, same as an if/for/while in a branch
 * that isn't being taken.
 *
 * Each clause opens with "PATTERN[|PATTERN...]) body..." -- possibly
 * spanning several plan entries if the pattern list itself uses "|" (see
 * shell_compound__collect_case_patterns() above) -- which settles *index on
 * the one entry that actually holds the glued body, right after the
 * closing ')'. This shrinks that one plan entry down to just the glued body
 * the same way shell_compound__consume_do() shrinks a "do" entry down to
 * its own glued remainder (reusing its shell_compound__do_span_t/
 * restore_do() -- the shape is identical, just produced by a different
 * keyword) -- and the body from
 * there is walked with shell_compound__run_sequence() exactly like a loop
 * body or if-branch is, so it can itself span multiple ;/&&/||-joined
 * commands, contain nested if/for/while/case constructs, all the way up to
 * the next ";;"-tagged entry (SHELL_CONNECT_CASE_END -- see its own doc
 * comment in shell_parser.h) or a bare "esac", whichever comes first --
 * including a *nested* case's own, since that recurses through the same
 * SHELL_COMPOUND_CASE dispatch in shell_compound__run_sequence() and fully
 * consumes its own clauses/"esac" before ever returning control here. */
static int shell_compound__run_case(shell_state_t *state, const shell_plan_t *plan, size_t *index, bool execute) {
    size_t header_index = *index;
    const char *rest;
    size_t rest_len;
    (void)shell_compound__match_keyword(&plan->commands[header_index], "case", &rest, &rest_len);

    size_t in_start;
    if (!shell_compound__find_case_in(rest, rest_len, &in_start)) {
        stdio__printf("shell: case: missing 'in'\n");
        state->last_status = 2;
        *index = plan->count;
        return 2;
    }
    const char *word_text = rest;
    size_t word_len = in_start;
    while (word_len > 0 && isspace((unsigned char)word_text[word_len - 1])) word_len--;

    size_t after_in = in_start + 2;
    const char *glued_text = rest + after_in;
    size_t glued_len = rest_len - after_in;
    while (glued_len > 0 && isspace((unsigned char)*glued_text)) {
        glued_text++;
        glued_len--;
    }

    char *word_value = NULL;
    if (execute) {
        const char *error = NULL;
        word_value = shell_parser__expand_text(
            word_text, word_len, shell_executor__lookup, shell_executor__run_substitution,
            shell_executor__eval_arith_word, state, state->last_status, &error
        );
        if (word_value == NULL) {
            stdio__printf("shell: case: %s\n", error != NULL ? error : "expansion error");
            state->last_status = 2;
            *index = plan->count;
            return 2;
        }
    }

    shell_compound__do_span_t header_span = {
        .index = header_index,
        .text = plan->commands[header_index].text,
        .length = plan->commands[header_index].length,
        .connector = plan->commands[header_index].connector,
    };
    if (glued_len > 0) {
        plan->commands[header_index].text = glued_text;
        plan->commands[header_index].length = glued_len;
        /* This entry -- now shrunk down to the first clause's own opener --
         * is about to be classified and (if it's itself a nested compound,
         * e.g. "case a in b) case ...") dispatched by run_sequence(), which
         * treats SHELL_CONNECT_CASE_END as "stop, this belongs to an
         * enclosing clause" (see that connector's own doc comment in
         * shell_parser.h). But that's only true of the *header* entry's
         * original connector -- how "case ..." followed whatever came
         * before it -- which is exactly what header_span above just saved
         * and shell_compound__restore_do() puts back once this shrink is no
         * longer needed; while it's in effect, this entry must read as an
         * ordinary sequential command so run_sequence() (both this
         * function's own clause loop below and, for a nested compound, its
         * dispatcher) actually runs/recurses into it instead of mistaking
         * it for the *outer* case's own next-clause boundary. */
        plan->commands[header_index].connector = SHELL_CONNECT_SEQUENCE;
        *index = header_index;
    } else {
        *index = header_index + 1;
    }

    bool matched = false;
    int status = 0;
    for (;;) {
        if (*index >= plan->count) {
            stdio__printf("shell: case: missing 'esac'\n");
            state->last_status = 2;
            status = 2;
            if (glued_len > 0) shell_compound__restore_do(plan, &header_span);
            memory__free(word_value);
            return status;
        }
        if (shell_compound__classify(plan, *index) == SHELL_COMPOUND_ESAC) {
            (*index)++;
            break;
        }

        shell_word_span_t patterns[SHELL__MAX_CASE_PATTERNS];
        size_t pattern_count = 0;
        size_t body_offset = 0;
        if (!shell_compound__collect_case_patterns(plan, index, patterns, &pattern_count, &body_offset)) {
            stdio__printf("shell: case: malformed clause (missing ')')\n");
            state->last_status = 2;
            if (glued_len > 0) shell_compound__restore_do(plan, &header_span);
            memory__free(word_value);
            *index = plan->count;
            return 2;
        }
        /* shell_compound__collect_case_patterns() may have walked *index
         * forward, past any purely-pattern PIPE-connected entries, onto the
         * one that actually holds the closing ')' -- re-fetch only now that
         * it's settled there. */
        const shell_command_t *clause_cmd = &plan->commands[*index];
        const char *body_text = clause_cmd->text + body_offset;
        size_t body_len = clause_cmd->length - body_offset;
        while (body_len > 0 && isspace((unsigned char)*body_text)) {
            body_text++;
            body_len--;
        }

        shell_compound__do_span_t clause_span = {
            .index = *index,
            .text = clause_cmd->text,
            .length = clause_cmd->length,
            .connector = plan->commands[*index].connector,
        };
        if (body_len > 0) {
            plan->commands[*index].text = body_text;
            plan->commands[*index].length = body_len;
            /* Same reasoning as header_span's own shrink above: this entry
             * is every non-first clause's own opener, which always carries
             * SHELL_CONNECT_CASE_END (it follows the previous clause's
             * ";;") -- clear it while the shrink is in effect so
             * run_sequence() runs/recurses into this clause's own body
             * instead of mistaking its first entry for a boundary and
             * stopping before ever executing (or even classifying) it;
             * shell_compound__restore_do() below puts the real connector
             * back once this clause's body has been fully walked. */
            plan->commands[*index].connector = SHELL_CONNECT_SEQUENCE;
        } else {
            (*index)++;
        }

        bool try_match = execute && !matched;
        bool clause_matches = false;
        bool expand_failed = false;
        for (size_t p = 0; try_match && !clause_matches && p < pattern_count; ++p) {
            const char *pattern_error = NULL;
            char *pattern_value = shell_parser__expand_text(
                patterns[p].text, patterns[p].length, shell_executor__lookup, shell_executor__run_substitution,
                shell_executor__eval_arith_word, state, state->last_status, &pattern_error
            );
            if (pattern_value == NULL) {
                stdio__printf("shell: case: %s\n", pattern_error != NULL ? pattern_error : "expansion error");
                expand_failed = true;
                break;
            }
            clause_matches = shell_glob__match(pattern_value, word_value);
            memory__free(pattern_value);
        }
        if (clause_matches) matched = true;

        if (!expand_failed) {
            int body_status = shell_compound__run_sequence(state, plan, index, clause_matches);
            shell_compound__catch_up(state, plan, index, clause_matches);
            if (clause_matches) status = body_status;
        }
        if (body_len > 0) shell_compound__restore_do(plan, &clause_span);
        if (expand_failed) {
            state->last_status = 2;
            if (glued_len > 0) shell_compound__restore_do(plan, &header_span);
            memory__free(word_value);
            *index = plan->count;
            return 2;
        }
    }

    if (glued_len > 0) shell_compound__restore_do(plan, &header_span);
    memory__free(word_value);
    return status;
}

int shell_compound__run(shell_state_t *state, const char *text, char *const *heredoc_bodies, size_t heredoc_count) {
    shell_plan_t plan = {0};
    const char *error = NULL;
    if (shell_parser__plan(text, &plan, heredoc_bodies, heredoc_count, &error) != 0) {
        shell_parser__plan_free(&plan);
        stdio__printf("shell: %s\n", error != NULL ? error : "syntax error");
        state->last_status = 2;
        return 2;
    }
    if (plan.count == 0) {
        shell_parser__plan_free(&plan);
        return 0;
    }
    size_t index = 0;
    int status = shell_compound__run_sequence(state, &plan, &index, true);
    /* A "break" that outlives every enclosing for/while loop in this run --
     * including one with no loop around it at all -- stops here rather than
     * silently reaching into whatever runs shell_compound__run() next. This
     * is also the boundary a function call (shell_compound__call_function())
     * runs its body through, so it doubles as the "break in a function
     * doesn't reach into the loop that called it" boundary bash itself
     * enforces. */
    bool stray_break = state->break_requested > 0;
    if (stray_break) {
        stdio__printf("shell: break: only meaningful inside a loop\n");
        state->break_requested = 0;
    }
    /* index<plan.count here means run_sequence stopped without reaching the
     * end of the plan -- ordinarily a real syntax problem (an orphaned
     * then/fi/done/"}" left over from a malformed construct), but also
     * exactly what a mid-plan "exit"/stray "break" leaves behind on
     * purpose, so those don't get misreported as one. */
    if (index < plan.count && !state->exit_requested && !stray_break) {
        stdio__printf(
            "shell: unexpected `%.*s'\n", (int)plan.commands[index].length, plan.commands[index].text
        );
        status = 2;
    }
    state->last_status = status;
    shell_parser__plan_free(&plan);
    return status;
}

bool shell_compound__pending(const char *text) {
    if (text == NULL) return false;
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    bool in_comment = false;
    int if_depth = 0;
    int brace_depth = 0;
    /* "for"/"while" both open a block that's closed by a matching "done"
     * (their "do" is just a delimiter word in between, not its own nesting
     * level -- same reason "then" needs no tracking of its own for if/fi).
     * One shared counter is enough since a stray "for"...(missing "done")
     * followed by a real "while"...done still needs a "done" of its own to
     * close, so nesting them under one counter and popping on any "done"
     * matches this the same way if_depth already treats "if" uniformly
     * regardless of elif/else in between. */
    int loop_depth = 0;
    /* "case" is closed by a matching "esac", the same one-counter-per-pair
     * treatment as if_depth/loop_depth above -- a clause's own ";;"/pattern
     * words never need tracking here, only the outermost case/esac pair. */
    int case_depth = 0;
    char word[8];
    size_t word_len = 0;
    bool word_clean = true;

    for (const char *p = text;; ++p) {
        char c = *p;
        bool end_of_buffer = c == '\0';
        if (!in_comment && !escaped && !single && !double_quote && (end_of_buffer || isspace((unsigned char)c))) {
            if (word_len > 0 && word_len < sizeof(word) && word_clean) {
                if (word_len == 2 && memcmp(word, "if", 2) == 0) if_depth++;
                else if (word_len == 2 && memcmp(word, "fi", 2) == 0 && if_depth > 0) if_depth--;
                else if (word_len == 1 && word[0] == '{') brace_depth++;
                else if (word_len == 1 && word[0] == '}' && brace_depth > 0) brace_depth--;
                else if (word_len == 3 && memcmp(word, "for", 3) == 0) loop_depth++;
                else if (word_len == 5 && memcmp(word, "while", 5) == 0) loop_depth++;
                else if (word_len == 4 && memcmp(word, "done", 4) == 0 && loop_depth > 0) loop_depth--;
                else if (word_len == 4 && memcmp(word, "case", 4) == 0) case_depth++;
                else if (word_len == 4 && memcmp(word, "esac", 4) == 0 && case_depth > 0) case_depth--;
            }
            word_len = 0;
            word_clean = true;
        }
        if (end_of_buffer) break;
        if (in_comment) {
            if (c == '\n') in_comment = false;
            continue;
        }
        if (escaped) {
            escaped = false;
            if (word_len < sizeof(word)) word[word_len++] = c;
            word_clean = false;
            continue;
        }
        if (!single && c == '\\') {
            escaped = true;
            word_clean = false;
            continue;
        }
        if (!double_quote && c == '\'') {
            single = !single;
            word_clean = false;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            word_clean = false;
            continue;
        }
        if (single || double_quote) {
            if (word_len < sizeof(word)) word[word_len++] = c;
            continue;
        }
        if (c == '#' && word_len == 0) {
            in_comment = true;
            continue;
        }
        if (isspace((unsigned char)c)) continue;
        if (word_len < sizeof(word)) word[word_len++] = c;
        else word_clean = false;
    }
    return single || double_quote || if_depth > 0 || brace_depth > 0 || loop_depth > 0 || case_depth > 0;
}

static shell_function_t *shell_compound__find_function(const shell_state_t *state, const char *name) {
    for (size_t i = 0; i < state->function_count; ++i) {
        if (strcmp(state->functions[i].name, name) == 0) return &state->functions[i];
    }
    return NULL;
}

bool shell_compound__is_function(const shell_state_t *state, const char *name) {
    return shell_compound__find_function(state, name) != NULL;
}

static int shell_compound__define_function(shell_state_t *state, const char *name, const char *body_text, size_t body_len) {
    while (body_len > 0 && isspace((unsigned char)body_text[0])) {
        body_text++;
        body_len--;
    }
    while (body_len > 0 && isspace((unsigned char)body_text[body_len - 1])) body_len--;
    if (body_len >= SHELL__FUNCTION_BODY_MAX) {
        stdio__printf("shell: %s: function body too long\n", name);
        return 1;
    }
    char *body = memory__malloc(body_len + 1);
    if (body == NULL) {
        stdio__printf("shell: out of memory\n");
        return 1;
    }
    memcpy(body, body_text, body_len);
    body[body_len] = '\0';

    /* Redefining a function while it's the one currently executing (it
     * redefines itself mid-call) would free the body string that call is
     * still reading from -- an accepted, unlikely-to-matter limitation for
     * this basic implementation, same as real shells' own edge cases around
     * self-redefinition. */
    shell_function_t *existing = shell_compound__find_function(state, name);
    if (existing != NULL) {
        memory__free(existing->body);
        existing->body = body;
        return 0;
    }
    if (state->function_count >= SHELL__MAX_FUNCTIONS) {
        stdio__printf("shell: function limit reached\n");
        memory__free(body);
        return 1;
    }
    if (state->function_count >= state->function_capacity) {
        size_t new_capacity = state->function_capacity == 0 ? 4u : state->function_capacity * 2u;
        if (new_capacity > SHELL__MAX_FUNCTIONS) new_capacity = SHELL__MAX_FUNCTIONS;
        shell_function_t *grown = memory__realloc(state->functions, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            stdio__printf("shell: out of memory\n");
            memory__free(body);
            return 1;
        }
        state->functions = grown;
        state->function_capacity = new_capacity;
    }
    size_t name_len = strlen(name);
    char *name_copy = memory__malloc(name_len + 1);
    if (name_copy == NULL) {
        stdio__printf("shell: out of memory\n");
        memory__free(body);
        return 1;
    }
    memcpy(name_copy, name, name_len + 1);
    state->functions[state->function_count].name = name_copy;
    state->functions[state->function_count].body = body;
    state->function_count++;
    return 0;
}

int shell_compound__call_function(shell_state_t *state, int argc, char **argv) {
    shell_function_t *fn = shell_compound__find_function(state, argv[0]);
    if (fn == NULL) return 127;

    int new_count = argc - 1;
    if (new_count > SHELL__MAX_POSITIONAL) new_count = SHELL__MAX_POSITIONAL;
    char **new_positional = NULL;
    if (new_count > 0) {
        new_positional = memory__malloc((size_t)new_count * sizeof(*new_positional));
        if (new_positional == NULL) {
            stdio__printf("shell: out of memory\n");
            return 1;
        }
        int allocated = 0;
        for (; allocated < new_count; ++allocated) {
            size_t len = strlen(argv[allocated + 1]);
            new_positional[allocated] = memory__malloc(len + 1);
            if (new_positional[allocated] == NULL) break;
            memcpy(new_positional[allocated], argv[allocated + 1], len + 1);
        }
        if (allocated < new_count) {
            for (int i = 0; i < allocated; ++i) memory__free(new_positional[i]);
            memory__free(new_positional);
            stdio__printf("shell: out of memory\n");
            return 1;
        }
    }

    /* fn->body is read out here, before the call -- if the function's own
     * body redefines it (see shell_compound__define_function()'s comment),
     * this call still runs to completion off the copy it already started
     * with. functions/function_count may still grow (realloc) during the
     * call, which is fine: `fn` itself is never dereferenced again below. */
    const char *body = fn->body;
    char **saved_positional = state->positional;
    int saved_positional_count = state->positional_count;
    const char *saved_arg0 = state->arg0;
    state->positional = new_positional;
    state->positional_count = new_count;
    state->arg0 = argv[0];

    /* frame lives on this call's own C stack frame, exactly like
     * saved_positional above -- shell_builtins__local() (see
     * shell_builtins.c) only ever writes into whatever state->local_frame
     * currently points at, so nested calls each get their own frame without
     * needing any explicit stack structure here: recursion into another
     * call_function() just points state->local_frame at *its* frame and
     * restores this one when that inner call returns, same as positional. */
    shell_local_frame_t frame = {0};
    shell_local_frame_t *saved_frame = state->local_frame;
    state->local_frame = &frame;

    /* NULL/0: a function body is re-parsed fresh from its stored text on
     * every call (see shell_compound__define_function() above), so any
     * heredoc marker in it has no body left to attach -- shell_parser__plan()
     * reports that plainly ("heredoc ... not supported here") rather than
     * this silently running the command with no stdin at all. Only the one
     * top-level shell_compound__run() call in shell_app.c's
     * shell__run_script() currently ever passes real heredoc bodies. */
    int status = shell_compound__run(state, body, NULL, 0);

    /* Unwind in reverse declaration order, restoring each localized name to
     * whatever it held just before this call's first "local NAME" -- or
     * removing it entirely, if it did not exist yet -- so a `local` here is
     * invisible again once the call that declared it returns. */
    for (size_t i = frame.count; i-- > 0;) {
        if (frame.entries[i].previous_value != NULL) {
            (void)shell_builtins__set(state, frame.entries[i].name, frame.entries[i].previous_value);
            memory__free(frame.entries[i].previous_value);
        } else {
            shell_builtins__unset(state, frame.entries[i].name);
        }
        memory__free(frame.entries[i].name);
    }
    memory__free(frame.entries);
    state->local_frame = saved_frame;

    for (int i = 0; i < state->positional_count; ++i) memory__free(state->positional[i]);
    memory__free(state->positional);
    state->positional = saved_positional;
    state->positional_count = saved_positional_count;
    state->arg0 = saved_arg0;
    state->last_status = status;
    return status;
}

void shell_compound__state_free(shell_state_t *state) {
    if (state == NULL) return;
    for (size_t i = 0; i < state->function_count; ++i) {
        memory__free(state->functions[i].name);
        memory__free(state->functions[i].body);
    }
    memory__free(state->functions);
    state->functions = NULL;
    state->function_count = 0;
    state->function_capacity = 0;
    for (int i = 0; i < state->positional_count; ++i) memory__free(state->positional[i]);
    memory__free(state->positional);
    state->positional = NULL;
    state->positional_count = 0;
}
