#include "html_test.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/html.h"
#include "core_sdk/result.h"

static bool html_test__resolve(const char *base, const char *ref, const char *expected) {
    char out[256];
    bool ok = html__resolve_url(base, ref, out, sizeof(out));
    if (!ok || strcmp(out, expected) != 0) {
        printf(
            "[selftest] html/url: FAIL, resolve(%s, %s) = %s (ok=%d), expected %s\n", base ? base : "(null)", ref,
            ok ? out : "<failed>", ok, expected
        );
        return false;
    }
    return true;
}

bool selftest__run_html_url_case(void) {
    bool ok = true;
    ok &= html_test__resolve("http://bruce.computer/", "about.html", "http://bruce.computer/about.html");
    ok &= html_test__resolve("http://bruce.computer", "about.html", "http://bruce.computer/about.html");
    ok &= html_test__resolve(
        "http://bruce.computer/dir/page.html", "other.html", "http://bruce.computer/dir/other.html"
    );
    ok &=
        html_test__resolve("http://bruce.computer/dir/page.html", "/root.html", "http://bruce.computer/root.html");
    ok &= html_test__resolve("http://bruce.computer/a/b/c.html", "../../x.html", "http://bruce.computer/x.html");
    ok &= html_test__resolve(
        "http://bruce.computer/dir/page.html", "https://example.com/x", "https://example.com/x"
    );
    ok &= html_test__resolve("http://bruce.computer/dir/page.html", "//example.com/x", "http://example.com/x");
    ok &= html_test__resolve(
        "http://bruce.computer/dir/page.html?x=1", "?y=2", "http://bruce.computer/dir/page.html?y=2"
    );
    ok &= html_test__resolve(NULL, "http://example.com/x", "http://example.com/x");

    char tiny[4];
    if (html__resolve_url("http://bruce.computer/", "way-too-long-for-this-buffer.html", tiny, sizeof(tiny))) {
        printf("[selftest] html/url: FAIL, should reject output that doesn't fit\n");
        ok = false;
    }
    if (html__resolve_url(NULL, "relative.html", tiny, sizeof(tiny))) {
        printf("[selftest] html/url: FAIL, should reject a relative ref with no base\n");
        ok = false;
    }

    printf("[selftest] html/url: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

typedef struct {
    char text[512];
    size_t text_len;
    char title[128];
    char link_url[256];
    char link_text[128];
    bool in_link;
    char image_url[256];
    char image_alt[128];
    int heading_starts;
    int heading_ends;
    int paragraph_breaks;
    int line_breaks;
    int link_count;
    int image_count;
    int anchor_count;
    char anchor[32];
} html_test__capture_t;

static void html_test__append(char *dst, size_t *len, size_t cap, const char *src, size_t src_len) {
    if (*len + src_len >= cap) return;
    memcpy(dst + *len, src, src_len);
    *len += src_len;
    dst[*len] = '\0';
}

static void html_test__on_event(const bruce_html_event_t *event, void *context) {
    html_test__capture_t *c = context;
    switch (event->type) {
    case BRUCE_HTML_EVENT_TEXT:
        html_test__append(c->text, &c->text_len, sizeof(c->text), event->text, event->text_len);
        if (c->in_link) {
            size_t n = strlen(c->link_text);
            html_test__append(c->link_text, &n, sizeof(c->link_text), event->text, event->text_len);
        }
        break;
    case BRUCE_HTML_EVENT_TITLE:
        snprintf(c->title, sizeof(c->title), "%.*s", (int)event->text_len, event->text);
        break;
    case BRUCE_HTML_EVENT_LINK_START:
        c->in_link = true;
        c->link_text[0] = '\0';
        snprintf(c->link_url, sizeof(c->link_url), "%.*s", (int)event->text_len, event->text);
        break;
    case BRUCE_HTML_EVENT_LINK_END:
        c->in_link = false;
        c->link_count++;
        break;
    case BRUCE_HTML_EVENT_IMAGE:
        c->image_count++;
        snprintf(c->image_url, sizeof(c->image_url), "%.*s", (int)event->text_len, event->text);
        snprintf(c->image_alt, sizeof(c->image_alt), "%.*s", (int)event->alt_len, event->alt ? event->alt : "");
        break;
    case BRUCE_HTML_EVENT_ANCHOR:
        c->anchor_count++;
        snprintf(c->anchor, sizeof(c->anchor), "%.*s", (int)event->text_len, event->text);
        break;
    case BRUCE_HTML_EVENT_HEADING_START:
        c->heading_starts++;
        break;
    case BRUCE_HTML_EVENT_HEADING_END:
        c->heading_ends++;
        break;
    case BRUCE_HTML_EVENT_PARAGRAPH_BREAK:
        c->paragraph_breaks++;
        break;
    case BRUCE_HTML_EVENT_LINE_BREAK:
        c->line_breaks++;
        break;
    }
}

/* Feeds `html` split across several small chunks (not one byte at a time,
 * unlike the state-machine's own offline unit tests) to exercise the same
 * cross-call continuation an HTTP chunk callback would trigger. */
static bruce_result_t html_test__parse(const char *base_url, const char *html, html_test__capture_t *out) {
    memset(out, 0, sizeof(*out));
    bruce_html_parser_t *parser = NULL;
    bruce_result_t result = html__parser_create(base_url, html_test__on_event, out, &parser);
    if (result != BRUCE_OK) return result;
    size_t len = strlen(html);
    size_t chunk = 7;
    for (size_t i = 0; i < len; i += chunk) {
        size_t n = (len - i) < chunk ? (len - i) : chunk;
        result = html__parser_feed(parser, html + i, n);
        if (result != BRUCE_OK) break;
    }
    if (result == BRUCE_OK) result = html__parser_finish(parser);
    html__parser_destroy(parser);
    return result;
}

bool selftest__run_html_parser_case(void) {
    html_test__capture_t c;
    bool ok = true;

    if (html_test__parse(
            "http://bruce.computer/",
            "<html><head><title>My  Page</title></head>"
            "<body><h1 id=\"faq\">Welcome</h1><p>Hello <b>World</b>! Visit "
            "<a href=\"/about\">our about page</a>.</p>"
            "<img src=\"logo.png\" alt=\"Bruce logo\"><script>if (1<2) bad();</script>done</body></html>",
            &c
        ) != BRUCE_OK) {
        ok = false;
    }
    if (strcmp(c.title, "My Page") != 0) {
        printf("[selftest] html/parser: FAIL, title = '%s'\n", c.title);
        ok = false;
    }
    if (c.anchor_count != 1 || strcmp(c.anchor, "faq") != 0) {
        printf("[selftest] html/parser: FAIL, anchor_count=%d anchor='%s'\n", c.anchor_count, c.anchor);
        ok = false;
    }
    if (strstr(c.text, "Hello World!") == NULL || strstr(c.text, "done") == NULL) {
        printf("[selftest] html/parser: FAIL, text = '%s'\n", c.text);
        ok = false;
    }
    if (strstr(c.text, "1<2") != NULL || strstr(c.text, "bad()") != NULL) {
        printf("[selftest] html/parser: FAIL, <script> content leaked into text: '%s'\n", c.text);
        ok = false;
    }
    if (c.link_count != 1 || strcmp(c.link_url, "http://bruce.computer/about") != 0 ||
        strcmp(c.link_text, "our about page") != 0) {
        printf(
            "[selftest] html/parser: FAIL, link_count=%d url='%s' text='%s'\n", c.link_count, c.link_url,
            c.link_text
        );
        ok = false;
    }
    if (c.image_count != 1 || strcmp(c.image_url, "http://bruce.computer/logo.png") != 0 ||
        strcmp(c.image_alt, "Bruce logo") != 0) {
        printf(
            "[selftest] html/parser: FAIL, image_count=%d url='%s' alt='%s'\n", c.image_count, c.image_url,
            c.image_alt
        );
        ok = false;
    }
    if (c.heading_starts != 1 || c.heading_ends != 1) {
        printf(
            "[selftest] html/parser: FAIL, heading_starts=%d heading_ends=%d\n", c.heading_starts, c.heading_ends
        );
        ok = false;
    }
    if (c.paragraph_breaks < 2) {
        printf("[selftest] html/parser: FAIL, paragraph_breaks=%d\n", c.paragraph_breaks);
        ok = false;
    }

    if (html_test__parse(NULL, "Tom &amp; Jerry &#39;quotes&#39; and &notreal;", &c) != BRUCE_OK) ok = false;
    if (strcmp(c.text, "Tom & Jerry 'quotes' and &notreal;") != 0) {
        printf("[selftest] html/parser: FAIL, entity decoding = '%s'\n", c.text);
        ok = false;
    }

    if (html_test__parse(NULL, "Line one<br>Line two", &c) != BRUCE_OK) ok = false;
    if (c.line_breaks != 1 || strcmp(c.text, "Line oneLine two") != 0) {
        printf("[selftest] html/parser: FAIL, line breaks/text = %d '%s'\n", c.line_breaks, c.text);
        ok = false;
    }

    /* Malformed markup must not hang or crash the tokenizer. */
    if (html_test__parse(NULL, "<a href unterminated <div><<< &&& \"\" <!-- unterminated", &c) != BRUCE_OK) {
        printf("[selftest] html/parser: FAIL, malformed document returned an error\n");
        ok = false;
    }

    printf("[selftest] html/parser: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
