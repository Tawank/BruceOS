#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "html_internal.h"

/* True if `s` begins with an RFC 3986 URI scheme ("http:", "mailto:", "data:",
 * ...); `*out_len` receives the scheme name length, not counting the colon. */
static bool html__has_scheme(const char *s, size_t *out_len) {
    if (!isalpha((unsigned char)s[0])) return false;
    size_t i = 1;
    while (s[i] != '\0' && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    if (s[i] != ':') return false;
    *out_len = i;
    return true;
}

/* Collapses "." and ".." path segments and preserves a trailing slash. Only
 * the path component (between the authority and any '?'/'#') is touched;
 * scheme, authority, query, and fragment pass through unchanged. Falls back
 * to copying `merged` unchanged if the path has more than 32 segments. */
static bool html__normalize_path(const char *merged, size_t merged_len, char *out_url, size_t out_capacity) {
    const char *sep = strstr(merged, "://");
    if (sep == NULL) {
        if (merged_len >= out_capacity) return false;
        memcpy(out_url, merged, merged_len + 1);
        return true;
    }
    const char *authority = sep + 3;
    const char *path_start = authority;
    while (*path_start != '\0' && *path_start != '/' && *path_start != '?' && *path_start != '#') path_start++;
    size_t prefix_len = (size_t)(path_start - merged);

    const char *tail_end = merged + merged_len;
    const char *path_end = path_start;
    while (path_end < tail_end && *path_end != '?' && *path_end != '#') path_end++;
    bool had_trailing_slash = (path_end > path_start) && (path_end[-1] == '/');

    const char *seg_start[32];
    size_t seg_len[32];
    size_t seg_count = 0;
    bool overflowed = false;

    const char *p = path_start;
    if (p < path_end && *p == '/') p++;
    const char *seg_begin = p;
    for (; p <= path_end; ++p) {
        if (p != path_end && *p != '/') continue;
        size_t len = (size_t)(p - seg_begin);
        if (len == 0 || (len == 1 && seg_begin[0] == '.')) {
            /* skip empty and "." segments */
        } else if (len == 2 && seg_begin[0] == '.' && seg_begin[1] == '.') {
            if (seg_count > 0) seg_count--;
        } else if (seg_count < sizeof(seg_start) / sizeof(seg_start[0])) {
            seg_start[seg_count] = seg_begin;
            seg_len[seg_count] = len;
            seg_count++;
        } else {
            overflowed = true;
        }
        seg_begin = p + 1;
    }

    if (overflowed) {
        if (merged_len >= out_capacity) return false;
        memcpy(out_url, merged, merged_len + 1);
        return true;
    }

    if (prefix_len >= out_capacity) return false;
    memcpy(out_url, merged, prefix_len);
    size_t out_len = prefix_len;
    for (size_t i = 0; i < seg_count; ++i) {
        if (out_len + 1 + seg_len[i] >= out_capacity) return false;
        out_url[out_len++] = '/';
        memcpy(out_url + out_len, seg_start[i], seg_len[i]);
        out_len += seg_len[i];
    }
    if (seg_count == 0 || had_trailing_slash) {
        if (out_len + 1 >= out_capacity) return false;
        out_url[out_len++] = '/';
    }
    size_t tail_len = (size_t)(tail_end - path_end);
    if (out_len + tail_len >= out_capacity) return false;
    memcpy(out_url + out_len, path_end, tail_len);
    out_len += tail_len;
    out_url[out_len] = '\0';
    return true;
}

bool html__resolve_url(const char *base_url, const char *ref, char *out_url, size_t out_capacity) {
    if (ref == NULL || out_url == NULL || out_capacity == 0) return false;
    size_t ref_len = strlen(ref);
    if (ref_len == 0) {
        if (base_url == NULL) return false;
        size_t n = strlen(base_url);
        if (n >= out_capacity) return false;
        memcpy(out_url, base_url, n + 1);
        return true;
    }

    size_t scheme_len = 0;
    if (html__has_scheme(ref, &scheme_len)) {
        if (ref_len >= out_capacity) return false;
        memcpy(out_url, ref, ref_len + 1);
        return true;
    }
    if (base_url == NULL || base_url[0] == '\0') return false;

    const char *sep = strstr(base_url, "://");
    if (sep == NULL) return false;
    size_t base_scheme_len = (size_t)(sep - base_url);
    const char *authority = sep + 3;
    const char *path_start = authority;
    while (*path_start != '\0' && *path_start != '/' && *path_start != '?' && *path_start != '#') path_start++;
    size_t authority_len = (size_t)(path_start - authority);
    size_t prefix_len = base_scheme_len + 3 + authority_len;

    char merged[HTML_URL_MAX];
    size_t merged_len;

    if (ref_len >= 2 && ref[0] == '/' && ref[1] == '/') {
        if (base_scheme_len + 1 + ref_len >= sizeof(merged)) return false;
        memcpy(merged, base_url, base_scheme_len);
        merged_len = base_scheme_len;
        merged[merged_len++] = ':';
        memcpy(merged + merged_len, ref, ref_len);
        merged_len += ref_len;
    } else if (ref[0] == '/') {
        if (prefix_len + ref_len >= sizeof(merged)) return false;
        memcpy(merged, base_url, prefix_len);
        merged_len = prefix_len;
        memcpy(merged + merged_len, ref, ref_len);
        merged_len += ref_len;
    } else if (ref[0] == '#' || ref[0] == '?') {
        /* A new fragment keeps the base's existing query string; a new query
         * discards both the old query and the now-stale old fragment. */
        const char *cut = ref[0] == '#' ? strchr(base_url, '#') : strchr(base_url, '?');
        size_t base_len = cut != NULL ? (size_t)(cut - base_url) : strlen(base_url);
        if (base_len + ref_len >= sizeof(merged)) return false;
        memcpy(merged, base_url, base_len);
        merged_len = base_len;
        memcpy(merged + merged_len, ref, ref_len);
        merged_len += ref_len;
    } else {
        const char *query_or_frag = strpbrk(path_start, "?#");
        const char *path_end = query_or_frag != NULL ? query_or_frag : base_url + strlen(base_url);
        const char *last_slash = NULL;
        for (const char *c = path_start; c < path_end; ++c) {
            if (*c == '/') last_slash = c;
        }
        size_t dir_len = last_slash != NULL ? (size_t)(last_slash + 1 - base_url) : prefix_len;
        bool need_slash = (dir_len == prefix_len);
        size_t total = dir_len + (need_slash ? 1u : 0u) + ref_len;
        if (total >= sizeof(merged)) return false;
        memcpy(merged, base_url, dir_len);
        merged_len = dir_len;
        if (need_slash) merged[merged_len++] = '/';
        memcpy(merged + merged_len, ref, ref_len);
        merged_len += ref_len;
    }
    merged[merged_len] = '\0';

    return html__normalize_path(merged, merged_len, out_url, out_capacity);
}
