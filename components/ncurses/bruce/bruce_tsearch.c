/* Minimal tsearch()/tdelete()/tfind() (POSIX <search.h>'s binary search
 * tree family) -- base/new_pair.c calls these unconditionally whenever
 * NCURSES_EXT_COLORS is defined, which include/curses.h always does (it's
 * part of the public API contract that header advertises, not something
 * this port should flip off), and tinfo/lib_tparm.c calls tsearch() too
 * when HAVE_TSEARCH is set. Neither is in the ELF loader's exported
 * symbol table, and whether picolibc's own <search.h> provides them for
 * this specific toolchain isn't something buildable/verifiable in the
 * environment this was written in (see README.md's "What's NOT yet
 * done") -- vendoring a small implementation here removes that
 * uncertainty entirely rather than hoping the toolchain has it.
 *
 * A plain, textbook unbalanced binary search tree -- not a copy of
 * glibc's or any other libc's tsearch.c, just the same well-known
 * standard algorithm and the same POSIX-specified signatures/behavior.
 * Fine for this use: both call sites use it as a lookup cache for a
 * bounded, small number of entries (per-screen color pairs, a small
 * recent-tparm-format cache), not as a workload that needs guaranteed
 * balance. */
#include <stddef.h>
#include <stdlib.h>

typedef struct bruce_tsearch_node {
    const void *key;
    struct bruce_tsearch_node *left;
    struct bruce_tsearch_node *right;
} bruce_tsearch_node_t;

void *tsearch(const void *key, void **rootp, int (*compar)(const void *, const void *)) {
    if (rootp == NULL) return NULL;

    bruce_tsearch_node_t **link = (bruce_tsearch_node_t **)rootp;
    while (*link != NULL) {
        int cmp = compar(key, (*link)->key);
        if (cmp == 0) return *link;
        link = cmp < 0 ? &(*link)->left : &(*link)->right;
    }

    bruce_tsearch_node_t *node = malloc(sizeof(*node));
    if (node == NULL) return NULL;
    node->key = key;
    node->left = NULL;
    node->right = NULL;
    *link = node;
    return node;
}

void *tfind(const void *key, void *const *rootp, int (*compar)(const void *, const void *)) {
    if (rootp == NULL) return NULL;

    bruce_tsearch_node_t *node = *(bruce_tsearch_node_t *const *)rootp;
    while (node != NULL) {
        int cmp = compar(key, node->key);
        if (cmp == 0) return node;
        node = cmp < 0 ? node->left : node->right;
    }
    return NULL;
}

/* Finds the in-order predecessor of `node` within its own left subtree
 * (the standard "rightmost node of the left subtree" BST-delete helper),
 * unlinking it from that subtree as it goes. */
static bruce_tsearch_node_t *bruce_tsearch__unlink_rightmost(bruce_tsearch_node_t **subtree) {
    bruce_tsearch_node_t **link = subtree;
    while ((*link)->right != NULL) link = &(*link)->right;

    bruce_tsearch_node_t *found = *link;
    *link = found->left;
    return found;
}

void *tdelete(const void *key, void **rootp, int (*compar)(const void *, const void *)) {
    if (rootp == NULL) return NULL;

    bruce_tsearch_node_t **link = (bruce_tsearch_node_t **)rootp;
    bruce_tsearch_node_t *parent_link_target = NULL; /* value to report back per POSIX */

    while (*link != NULL) {
        int cmp = compar(key, (*link)->key);
        if (cmp == 0) break;
        link = cmp < 0 ? &(*link)->left : &(*link)->right;
    }
    if (*link == NULL) return NULL; /* not found */

    bruce_tsearch_node_t *doomed = *link;
    if (doomed->left == NULL) {
        *link = doomed->right;
    } else if (doomed->right == NULL) {
        *link = doomed->left;
    } else {
        bruce_tsearch_node_t *replacement = bruce_tsearch__unlink_rightmost(&doomed->left);
        replacement->left = doomed->left;
        replacement->right = doomed->right;
        *link = replacement;
    }

    /* POSIX: returns a pointer to the parent of the deleted node, or
     * (implementation-defined when the tree is now empty) some non-NULL
     * value -- ncurses' own two call sites never inspect this return
     * value, only whether tsearch()/tfind() found something, so an
     * address that merely isn't NULL is sufficient here. */
    parent_link_target = (bruce_tsearch_node_t *)rootp;
    free(doomed);
    return parent_link_target;
}
