#include "browser_debug.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "browser_layout.h"
#include "browser_render.h"
#include "core_sdk/display.h"

static void browser_debug__metrics(int16_t *char_width, int16_t *char_height) {
    /* Mirrors browser_render__metrics()'s fallback -- kept as a separate copy
     * rather than exposed from browser_render.c since this is the only other
     * place that needs it. */
    if (display__get_font_metrics(char_width, char_height) != BRUCE_OK || *char_width <= 0 || *char_height <= 0) {
        *char_width = 6;
        *char_height = 8;
    }
}

static const char *browser_debug__kind_name(uint8_t kind) {
    switch (kind) {
    case BROWSER_ITEM_TEXT: return "TEXT";
    case BROWSER_ITEM_IMAGE: return "IMAGE";
    case BROWSER_ITEM_LINE_BREAK: return "BREAK";
    case BROWSER_ITEM_PARAGRAPH_BREAK: return "PARA_BREAK";
    default: return "?";
    }
}

static void browser_debug__dump_items(const browser_document_t *doc) {
    printf(
        "[browser_debug] document: url=\"%s\" title=\"%s\" truncated=%s\n", doc->url, doc->title,
        doc->truncated ? "true" : "false"
    );
    printf(
        "[browser_debug] items=%zu/%zu links=%zu images=%zu text_pool=%zu bytes\n", doc->item_count, doc->item_cap,
        doc->link_count, doc->image_count, doc->text_pool_len
    );
    for (size_t i = 0; i < doc->item_count; ++i) {
        const browser_item_t *item = &doc->items[i];
        printf(
            "[browser_debug]   item[%3zu] %-10s heading=%d link=%d image=%d", i, browser_debug__kind_name(item->kind),
            item->heading_level, item->link_index, item->image_index
        );
        if (item->kind == BROWSER_ITEM_TEXT) {
            printf(" text=\"%.*s\"", (int)item->text_len, doc->text_pool + item->text_offset);
        }
        if (item->link_index >= 0 && (size_t)item->link_index < doc->link_count) {
            printf(" link_url=\"%s\"", doc->links[item->link_index].url);
        }
        if (item->kind == BROWSER_ITEM_IMAGE && item->image_index >= 0 &&
            (size_t)item->image_index < doc->image_count) {
            const browser_image_ref_t *image = &doc->images[item->image_index];
            printf(" image_url=\"%s\" alt=\"%s\"", image->url, image->alt);
        }
        printf("\n");
    }
}

typedef struct {
    size_t index;
} browser_debug__layout_ctx_t;

static void browser_debug__layout_visitor(const browser_layout_token_t *token, void *context) {
    browser_debug__layout_ctx_t *ctx = context;
    if (token->image_index >= 0) {
        printf(
            "[browser_debug]   token[%3zu] IMAGE  x=%-4d y=%-5d h=%-3d image_index=%d\n", ctx->index, token->x,
            token->y, token->line_height, token->image_index
        );
    } else {
        printf(
            "[browser_debug]   token[%3zu] TEXT   x=%-4d y=%-5d h=%-3d heading=%-2d link=%-3d text=\"%.*s\"\n",
            ctx->index, token->x, token->y, token->line_height, token->heading_level, token->link_index,
            (int)token->text_len, token->text
        );
    }
    ctx->index++;
}

static void browser_debug__dump_layout(const browser_document_t *doc, int font_scale) {
    int16_t char_width, char_height;
    browser_debug__metrics(&char_width, &char_height);
    int width = browser_render__content_width();
    printf("[browser_debug] layout: width=%d char=%dx%d font_scale=%d\n", width, char_width, char_height, font_scale);

    browser_debug__layout_ctx_t ctx = {.index = 0};
    int height =
        browser_layout__walk(doc, width, char_width, char_height, font_scale, browser_debug__layout_visitor, &ctx);
    printf("[browser_debug] layout: %zu tokens, content_height=%d\n", ctx.index, height);
}

void browser_debug__dump(const browser_document_t *doc, int font_scale) {
    if (doc == NULL) {
        printf("[browser_debug] dump: doc == NULL\n");
        return;
    }
    browser_debug__dump_items(doc);
    browser_debug__dump_layout(doc, font_scale);
}
