#include "shell_glob.h"

#include <stdlib.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/storage.h"

/* Runaway-expansion guard on the total number of paths one
 * shell_glob__expand_path() call can return (or carry as an intermediate
 * directory frontier partway through a multi-component pattern) -- not a
 * realistic script's needs, just a bound so a pattern with several
 * wildcarded path components chained together can't grow without limit
 * over a large tree on this device. Hitting it just silently stops
 * collecting further matches (see shell_glob__path_list_push() below), the
 * same way shell_glob__expand_path() reports "nothing matched" for any
 * other reason -- not a separate error. */
#define SHELL_GLOB__MAX_MATCHES 256

/* Matches one glob `pattern` (a plain, already-expanded NUL-terminated
 * string) against a `]`-terminated bracket expression starting at
 * pattern[0] == '['. See shell_glob__match() below for the member/negation/
 * range/leading-']' rules; *out_valid is set false (with `c`'s match result
 * meaningless) when this '[' has no closing ']' at all, so the caller falls
 * back to treating it as one ordinary literal character. On a valid class,
 * *out_end is set just past the closing ']'. */
static bool shell_glob__class(const char *pattern, char c, const char **out_end, bool *out_valid) {
    const char *p = pattern + 1;
    bool negate = *p == '!' || *p == '^';
    if (negate) p++;
    const char *body = p;
    const char *scan = *p == ']' ? p + 1 : p;
    while (*scan != '\0' && *scan != ']') scan++;
    if (*scan != ']') {
        *out_valid = false;
        return false;
    }
    *out_end = scan + 1;
    *out_valid = true;
    bool matched = false;
    for (const char *q = body; q < scan;) {
        if (q + 2 < scan && q[1] == '-') {
            if ((unsigned char)c >= (unsigned char)q[0] && (unsigned char)c <= (unsigned char)q[2]) matched = true;
            q += 3;
        } else {
            if (c == *q) matched = true;
            q += 1;
        }
    }
    return negate ? !matched : matched;
}

bool shell_glob__match(const char *pattern, const char *text) {
    const char *p = pattern;
    const char *t = text;
    const char *star_p = NULL;
    const char *star_t = NULL;
    while (*t != '\0') {
        bool one_matches;
        size_t consumed = 1;
        if (*p == '[') {
            const char *class_end = NULL;
            bool valid = false;
            bool class_matched = shell_glob__class(p, *t, &class_end, &valid);
            if (valid) {
                one_matches = class_matched;
                consumed = (size_t)(class_end - p);
            } else {
                one_matches = *p == *t;
            }
        } else if (*p == '?') {
            one_matches = true;
        } else if (*p == '*') {
            star_p = p;
            star_t = t;
            p++;
            continue;
        } else {
            one_matches = *p != '\0' && *p == *t;
        }
        if (one_matches) {
            p += consumed;
            t++;
            continue;
        }
        if (star_p == NULL) return false;
        p = star_p + 1;
        star_t++;
        t = star_t;
    }
    while (*p == '*') p++;
    return *p == '\0';
}

bool shell_glob__has_metachars(const char *word) {
    if (word == NULL) return false;
    for (const char *p = word; *p != '\0'; ++p) {
        if (*p == '*' || *p == '?') return true;
        /* '[' only counts when some later ']' could plausibly close it --
         * this is still just a fast pre-filter (shell_glob__class() is what
         * actually validates the bracket expression, falling back to a
         * literal '[' itself when it doesn't), but without this check, a
         * bare "[", "[[", "]", or "]]" -- extremely common as a whole word
         * in its own right, being the test/[/[[ builtins' own names -- would
         * trigger a real, always-fruitless filesystem walk on nearly every
         * such invocation. */
        if (*p == '[') {
            for (const char *q = p + 1; *q != '\0'; ++q) {
                if (*q == ']') return true;
            }
        }
    }
    return false;
}

static char *shell_glob__dup(const char *text) {
    size_t length = strlen(text);
    char *copy = memory__malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

/* Strips the last '/'-component off `path` (a ".." step), clamped at root
 * -- "/apps" -> "/", "/apps/sub" -> "/apps", "/" -> "/". Heap-allocated,
 * caller-owned. */
static char *shell_glob__parent(const char *path) {
    size_t length = strlen(path);
    size_t end = length;
    while (end > 0 && path[end - 1] != '/') end--;
    /* `end` is now just past the last '/' still in `path` (or 0 if there is
     * none at all, which shouldn't happen for any path this module builds --
     * every candidate is always at least "/"). Dropping that trailing '/'
     * itself leaves the parent; at or below root, there's nowhere higher to
     * go. */
    if (end <= 1) return shell_glob__dup("/");
    size_t new_length = end - 1;
    char *parent = memory__malloc(new_length + 1);
    if (parent == NULL) return NULL;
    memcpy(parent, path, new_length);
    parent[new_length] = '\0';
    return parent;
}

/* A plain list of heap-allocated result strings -- used only for `results`
 * below, the final answer text shell_glob__expand_path() hands back. */
typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} shell_glob__path_list_t;

/* Takes ownership of `path` (heap-allocated, or NULL on OOM upstream, which
 * this reports as failure). Returns false only for a real allocation
 * failure -- hitting SHELL_GLOB__MAX_MATCHES just silently drops `path`
 * and returns true, matching this module's own "trim, don't error" cap
 * policy (see shell_glob.h). */
static bool shell_glob__path_list_push(shell_glob__path_list_t *list, char *path) {
    if (path == NULL) return false;
    if (list->count >= SHELL_GLOB__MAX_MATCHES) {
        memory__free(path);
        return true;
    }
    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        if (new_capacity > SHELL_GLOB__MAX_MATCHES) new_capacity = SHELL_GLOB__MAX_MATCHES;
        char **grown = memory__realloc(list->items, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            memory__free(path);
            return false;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = path;
    return true;
}

static void shell_glob__path_list_free(shell_glob__path_list_t *list) {
    for (size_t i = 0; i < list->count; ++i) memory__free(list->items[i]);
    memory__free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* One directory this module is still walking down, carried as a pair: what
 * to actually storage__list() (`resolved`, always absolute) alongside what
 * to show in the final result (`display`, mirroring the literal prefix the
 * pattern itself was written with -- "" for a relative pattern with no
 * literal directory component yet, so the first wildcard match's own name
 * becomes the whole result instead of $PWD's absolute path getting prepended
 * to it, same as bash's own "echo *.txt" -> "a.txt", not "/cwd/a.txt"). */
typedef struct {
    char *resolved;
    char *display;
} shell_glob__candidate_t;

typedef struct {
    shell_glob__candidate_t *items;
    size_t count;
    size_t capacity;
} shell_glob__candidate_list_t;

/* Takes ownership of both `resolved` and `display` (either/both NULL on OOM
 * upstream, which this reports as failure). Same cap/failure policy as
 * shell_glob__path_list_push() above. */
static bool shell_glob__candidate_list_push(shell_glob__candidate_list_t *list, char *resolved, char *display) {
    if (resolved == NULL || display == NULL) {
        memory__free(resolved);
        memory__free(display);
        return false;
    }
    if (list->count >= SHELL_GLOB__MAX_MATCHES) {
        memory__free(resolved);
        memory__free(display);
        return true;
    }
    if (list->count >= list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        if (new_capacity > SHELL_GLOB__MAX_MATCHES) new_capacity = SHELL_GLOB__MAX_MATCHES;
        shell_glob__candidate_t *grown = memory__realloc(list->items, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            memory__free(resolved);
            memory__free(display);
            return false;
        }
        list->items = grown;
        list->capacity = new_capacity;
    }
    list->items[list->count].resolved = resolved;
    list->items[list->count].display = display;
    list->count++;
    return true;
}

static void shell_glob__candidate_list_free(shell_glob__candidate_list_t *list) {
    for (size_t i = 0; i < list->count; ++i) {
        memory__free(list->items[i].resolved);
        memory__free(list->items[i].display);
    }
    memory__free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

/* Joins `dir` ("/" or a path with no trailing '/') and `name` into a fresh
 * "dir/name" path ("/name" when dir is root). Heap-allocated, caller-owned;
 * NULL on OOM. */
static char *shell_glob__join(const char *dir, const char *name) {
    size_t dir_length = strlen(dir);
    bool root = dir_length == 1 && dir[0] == '/';
    size_t name_length = strlen(name);
    size_t total = (root ? 1 : dir_length + 1) + name_length + 1;
    char *joined = memory__malloc(total);
    if (joined == NULL) return NULL;
    if (root) {
        joined[0] = '/';
        memcpy(joined + 1, name, name_length + 1);
    } else {
        memcpy(joined, dir, dir_length);
        joined[dir_length] = '/';
        memcpy(joined + dir_length + 1, name, name_length + 1);
    }
    return joined;
}

/* Like shell_glob__join() above, but for a *display* prefix, where an empty
 * `prefix` means "nothing written yet" rather than "root" -- so the result
 * is just `name` on its own, with no leading '/' at all. */
static char *shell_glob__join_display(const char *prefix, const char *name) {
    if (prefix == NULL || prefix[0] == '\0') return shell_glob__dup(name);
    return shell_glob__join(prefix, name);
}

/* Lists `resolved` and matches every entry against `segment` (glob-matched
 * via shell_glob__match() when it has a metacharacter, exact-compared
 * otherwise), applying the dotfile-hiding default (a wildcard segment skips
 * a '.'-leading name unless the segment itself starts with '.'; an exact
 * literal segment always matches normally, dotfile or not). A match becomes
 * a result (any type) when `is_last`, or -- when there's more pattern left
 * -- a new frontier candidate, but only if it's actually a directory (can't
 * descend into a file); `display` is this candidate's own display prefix
 * (see shell_glob__candidate_t's doc comment), joined with the matched
 * name either way. `resolved` failing to list at all (doesn't exist, isn't
 * a directory, permission denied, ...) is not an error here, just zero
 * matches from this branch, matching bash's own silent skip of an
 * unreadable directory mid-glob. Returns false only on a real allocation
 * failure. */
static bool shell_glob__scan_dir(
    const char *resolved, const char *display, const char *segment, bool is_last, shell_glob__path_list_t *results,
    shell_glob__candidate_list_t *next
) {
    bool wild = shell_glob__has_metachars(segment);
    size_t count = 0;
    if (storage__list(resolved, NULL, 0, &count) != BRUCE_OK || count == 0) return true;
    bruce_storage_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return false;
    bruce_result_t result = storage__list(resolved, entries, count, &count);
    bool ok = true;
    if (result == BRUCE_OK) {
        for (size_t i = 0; i < count && ok; ++i) {
            const char *name = entries[i].name;
            bool hidden = name[0] == '.';
            bool matches =
                wild ? (!hidden || segment[0] == '.') && shell_glob__match(segment, name) : strcmp(segment, name) == 0;
            if (!matches) continue;
            if (is_last) {
                ok = shell_glob__path_list_push(results, shell_glob__join_display(display, name));
                continue;
            }
            if (entries[i].type != BRUCE_STORAGE_ENTRY_DIRECTORY) continue; /* matched a file mid-path -- nothing to descend into */
            char *joined_resolved = shell_glob__join(resolved, name);
            char *joined_display = shell_glob__join_display(display, name);
            ok = shell_glob__candidate_list_push(next, joined_resolved, joined_display);
        }
    }
    memory__free(entries);
    return ok;
}

/* One '/'-separated component of `pattern`, applied to every directory
 * currently in `candidates`: "." and ".." resolve `resolved` by direct path
 * manipulation (see shell_glob__parent()'s own doc comment) since
 * storage__list() never returns pseudo-entries for either, while `display`
 * just gets the literal segment text appended -- bash's own glob doesn't
 * collapse a written-out "." or ".." in its result text either. Anything
 * else goes through shell_glob__scan_dir() above. Returns false only on a
 * real allocation failure. */
static bool shell_glob__step(
    const shell_glob__candidate_list_t *candidates, const char *segment, bool is_last, shell_glob__path_list_t *results,
    shell_glob__candidate_list_t *next
) {
    bool is_dot = strcmp(segment, ".") == 0;
    bool is_dotdot = strcmp(segment, "..") == 0;
    for (size_t i = 0; i < candidates->count; ++i) {
        const char *resolved = candidates->items[i].resolved;
        const char *display = candidates->items[i].display;
        if (is_dot || is_dotdot) {
            char *joined_resolved = is_dot ? shell_glob__dup(resolved) : shell_glob__parent(resolved);
            char *joined_display = shell_glob__join_display(display, segment);
            if (is_last) {
                memory__free(joined_resolved);
                if (!shell_glob__path_list_push(results, joined_display)) return false;
            } else if (!shell_glob__candidate_list_push(next, joined_resolved, joined_display)) {
                return false;
            }
            continue;
        }
        if (!shell_glob__scan_dir(resolved, display, segment, is_last, results, next)) return false;
    }
    return true;
}

static int shell_glob__compare(const void *a, const void *b) {
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

char **shell_glob__expand_path(const char *pattern, const char *pwd, size_t *out_count) {
    *out_count = 0;
    if (pattern == NULL || pattern[0] == '\0') return NULL;
    size_t pattern_length = strlen(pattern);

    shell_glob__candidate_list_t candidates = {0};
    shell_glob__path_list_t results = {0};
    size_t i;
    char *base_resolved;
    char *base_display;
    if (pattern[0] == '/') {
        base_resolved = shell_glob__dup("/");
        base_display = shell_glob__dup("/");
        i = 1;
    } else {
        base_resolved = shell_glob__dup(pwd != NULL && pwd[0] != '\0' ? pwd : "/");
        base_display = shell_glob__dup("");
        i = 0;
    }
    if (!shell_glob__candidate_list_push(&candidates, base_resolved, base_display)) {
        shell_glob__candidate_list_free(&candidates);
        return NULL;
    }

    bool ok = true;
    while (ok && i <= pattern_length && candidates.count > 0) {
        size_t seg_end = i;
        while (seg_end < pattern_length && pattern[seg_end] != '/') seg_end++;
        if (seg_end == i) {
            /* Empty component -- a leading, doubled, or trailing '/'. */
            i = seg_end + 1;
            continue;
        }
        size_t seg_len = seg_end - i;
        if (seg_len >= BRUCE_STORAGE_NAME_MAX) {
            ok = false;
            break;
        }
        char segment[BRUCE_STORAGE_NAME_MAX];
        memcpy(segment, pattern + i, seg_len);
        segment[seg_len] = '\0';

        size_t after = seg_end;
        while (after < pattern_length && pattern[after] == '/') after++;
        bool is_last = after >= pattern_length;

        shell_glob__candidate_list_t next = {0};
        ok = shell_glob__step(&candidates, segment, is_last, &results, &next);
        shell_glob__candidate_list_free(&candidates);
        candidates = next;
        i = seg_end + 1;
    }
    shell_glob__candidate_list_free(&candidates);

    if (!ok || results.count == 0) {
        shell_glob__path_list_free(&results);
        return NULL;
    }
    qsort(results.items, results.count, sizeof(*results.items), shell_glob__compare);
    *out_count = results.count;
    return results.items;
}

void shell_glob__free_matches(char **matches, size_t count) {
    if (matches == NULL) return;
    for (size_t i = 0; i < count; ++i) memory__free(matches[i]);
    memory__free(matches);
}
