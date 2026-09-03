#include "shell_brace.h"

#include <string.h>

#include "core_sdk/memory.h"

/* Runaway-expansion guards -- not a realistic script's needs, just bounds so
 * a pathological word ("{a,b}{c,d}{e,f}{g,h}..." chained many times over, or
 * deeply nested) can't grow without limit. Hitting either produces a real
 * error (unlike shell_glob.c's own "silently trim, don't error" cap): a
 * brace group is pure, self-contained syntax the caller wrote out by hand
 * (not something filesystem-dependent), so a script that actually needs more
 * than this is almost certainly a mistake worth surfacing rather than
 * silently truncating. */
#define SHELL_BRACE__MAX_ALTS 64
#define SHELL_BRACE__MAX_DEPTH 8

/* Scans `text[0..length)` for the first unquoted, unescaped "{...}" group
 * (tracking its own nested brace depth and quote state so an inner group's
 * '{'/'}'/',' don't get mistaken for the outer one's) that has at least one
 * top-level ',' inside it -- the only shape this shell treats as a real
 * brace group, same as shell_brace__has_group()'s own doc comment. A
 * "{...}" with no matching '}' at all, or no top-level comma, is left
 * completely alone (not even reported as found) and the scan continues past
 * it, in case a later group in the same word does qualify. */
static bool shell_brace__find_group(const char *text, size_t length, size_t *out_open, size_t *out_close) {
    size_t i = 0;
    bool single = false;
    bool double_quote = false;
    while (i < length) {
        char c = text[i];
        if (!double_quote && c == '\'') {
            single = !single;
            i++;
            continue;
        }
        if (!single && c == '"') {
            double_quote = !double_quote;
            i++;
            continue;
        }
        if (!single && !double_quote && c == '\\') {
            i += 2;
            continue;
        }
        if (single || double_quote || c != '{') {
            i++;
            continue;
        }
        size_t depth = 1;
        bool has_comma = false;
        bool inner_single = false;
        bool inner_double = false;
        size_t close = (size_t)-1;
        size_t j = i + 1;
        while (j < length) {
            char cj = text[j];
            if (!inner_double && cj == '\'') {
                inner_single = !inner_single;
                j++;
                continue;
            }
            if (!inner_single && cj == '"') {
                inner_double = !inner_double;
                j++;
                continue;
            }
            if (!inner_single && !inner_double && cj == '\\') {
                j += 2;
                continue;
            }
            if (inner_single || inner_double) {
                j++;
                continue;
            }
            if (cj == '{') {
                depth++;
                j++;
                continue;
            }
            if (cj == '}') {
                depth--;
                j++;
                if (depth == 0) {
                    close = j - 1;
                    break;
                }
                continue;
            }
            if (cj == ',' && depth == 1) has_comma = true;
            j++;
        }
        if (close != (size_t)-1 && has_comma) {
            *out_open = i;
            *out_close = close;
            return true;
        }
        /* Unterminated, or no top-level comma -- not a group; keep looking
         * for another '{' later in the text. */
        i++;
    }
    return false;
}

bool shell_brace__has_group(const char *text, size_t length) {
    size_t open = 0;
    size_t close = 0;
    return shell_brace__find_group(text, length, &open, &close);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} shell_brace__list_t;

/* Takes ownership of `text` (or NULL on OOM upstream, reported as failure).
 * Returns false (with *error set) on out-of-memory or on exceeding
 * SHELL_BRACE__MAX_ALTS. */
static bool shell_brace__list_push(shell_brace__list_t *list, char *text, const char **error) {
    if (text == NULL) {
        *error = "out of memory";
        return false;
    }
    if (list->count >= SHELL_BRACE__MAX_ALTS) {
        memory__free(text);
        *error = "too many brace alternatives";
        return false;
    }
    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        char **grown = memory__realloc(list->items, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            memory__free(text);
            *error = "out of memory";
            return false;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = text;
    return true;
}

static void shell_brace__list_free(shell_brace__list_t *list) {
    for (size_t i = 0; i < list->count; ++i) memory__free(list->items[i]);
    memory__free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* Concatenates three byte ranges into one fresh, NUL-terminated,
 * heap-allocated string. NULL on OOM. */
static char *shell_brace__concat3(
    const char *a, size_t a_len, const char *b, size_t b_len, const char *c, size_t c_len
) {
    char *out = memory__malloc(a_len + b_len + c_len + 1);
    if (out == NULL) return NULL;
    memcpy(out, a, a_len);
    memcpy(out + a_len, b, b_len);
    memcpy(out + a_len + b_len, c, c_len);
    out[a_len + b_len + c_len] = '\0';
    return out;
}

static bool shell_brace__expand_depth(const char *text, size_t length, shell_brace__list_t *out, int depth, const char **error) {
    size_t open = 0;
    size_t close = 0;
    if (!shell_brace__find_group(text, length, &open, &close)) {
        char *copy = memory__malloc(length + 1);
        if (copy == NULL) {
            *error = "out of memory";
            return false;
        }
        memcpy(copy, text, length);
        copy[length] = '\0';
        return shell_brace__list_push(out, copy, error);
    }
    if (depth >= SHELL_BRACE__MAX_DEPTH) {
        *error = "brace expansion nested too deep";
        return false;
    }
    const char *prefix = text;
    size_t prefix_len = open;
    const char *suffix = text + close + 1;
    size_t suffix_len = length - (close + 1);
    size_t inner_start = open + 1;
    size_t inner_end = close;

    size_t seg_start = inner_start;
    bool single = false;
    bool double_quote = false;
    size_t nest = 0;
    size_t k = inner_start;
    bool ok = true;
    while (ok && k <= inner_end) {
        bool boundary = false;
        if (k == inner_end) {
            boundary = true;
        } else {
            char c = text[k];
            if (!double_quote && c == '\'') {
                single = !single;
                k++;
                continue;
            }
            if (!single && c == '"') {
                double_quote = !double_quote;
                k++;
                continue;
            }
            if (!single && !double_quote && c == '\\') {
                k += 2;
                continue;
            }
            if (single || double_quote) {
                k++;
                continue;
            }
            if (c == '{') {
                nest++;
                k++;
                continue;
            }
            if (c == '}') {
                nest--;
                k++;
                continue;
            }
            boundary = c == ',' && nest == 0;
        }
        if (boundary) {
            size_t seg_len = k - seg_start;
            char *combined = shell_brace__concat3(prefix, prefix_len, text + seg_start, seg_len, suffix, suffix_len);
            if (combined == NULL) {
                *error = "out of memory";
                ok = false;
                break;
            }
            ok = shell_brace__expand_depth(combined, prefix_len + seg_len + suffix_len, out, depth + 1, error);
            memory__free(combined);
            seg_start = k + 1;
        }
        k++;
    }
    return ok;
}

bool shell_brace__expand(const char *text, size_t length, char ***out_variants, size_t *out_count, const char **error) {
    *out_variants = NULL;
    *out_count = 0;
    shell_brace__list_t list = {0};
    if (!shell_brace__expand_depth(text, length, &list, 0, error)) {
        shell_brace__list_free(&list);
        return false;
    }
    *out_variants = list.items;
    *out_count = list.count;
    return true;
}

void shell_brace__free(char **variants, size_t count) {
    if (variants == NULL) return;
    for (size_t i = 0; i < count; ++i) memory__free(variants[i]);
    memory__free(variants);
}
