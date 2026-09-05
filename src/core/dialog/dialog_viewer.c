/* dialog__create_text_viewer()/_set_text()/_scroll()/_set_text_size()/_close()
 * -- a small pool of resettable scrollable-text viewers (see core_sdk/dialog.h
 * for the full contract). Self-contained: nothing else in core/dialog/ calls
 * into this file. */

#include "dialog_gui_common.h"
#include "dialog_internal.h"

#include <stdbool.h>
#include <string.h>

#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"

#include "core/process/process.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DIALOG__CHAR_W 6
#define DIALOG__CHAR_H 10
#define DIALOG__TEXT_SIZE 1
#define DIALOG__MARGIN 2
#define DIALOG__VIEWER_MAX 4

typedef struct {
    bool used;
    bruce_process_id_t owner;
    bruce_resource_id_t resource_id;
    char *title;
    char *text;
    int scroll_y;
    int text_size;
} dialog__viewer_t;

static dialog__viewer_t s_viewers[DIALOG__VIEWER_MAX];
static StaticSemaphore_t s_viewer_mutex_storage;
static SemaphoreHandle_t s_viewer_mutex;

static void dialog__viewer_lock(void) {
    if (s_viewer_mutex != NULL) { xSemaphoreTake(s_viewer_mutex, portMAX_DELAY); }
}

static void dialog__viewer_unlock(void) {
    if (s_viewer_mutex != NULL) { xSemaphoreGive(s_viewer_mutex); }
}

static char *dialog__strdup(const char *src) {
    if (src == NULL) { src = ""; }
    size_t len = strlen(src);
    char *copy = memory__malloc(len + 1);
    if (copy == NULL) { return NULL; }
    memcpy(copy, src, len + 1);
    return copy;
}

static dialog__viewer_t *dialog__viewer_find(bruce_viewer_id_t id) {
    if (id == BRUCE_VIEWER_ID_INVALID || id > DIALOG__VIEWER_MAX) { return NULL; }
    dialog__viewer_t *viewer = &s_viewers[id - 1];
    return viewer->used ? viewer : NULL;
}

static void dialog__viewer_free(dialog__viewer_t *viewer) {
    if (viewer == NULL) { return; }
    memory__free(viewer->title);
    memory__free(viewer->text);
    viewer->title = NULL;
    viewer->text = NULL;
    viewer->used = false;
    viewer->owner = BRUCE_PROCESS_ID_INVALID;
    viewer->resource_id = BRUCE_RESOURCE_ID_INVALID;
    viewer->scroll_y = 0;
    viewer->text_size = DIALOG__TEXT_SIZE;
}

static void dialog__viewer_cleanup(void *context) {
    dialog__viewer_t *viewer = (dialog__viewer_t *)context;
    dialog__viewer_lock();
    dialog__viewer_free(viewer);
    dialog__viewer_unlock();
}

/* Full-bleed title bar filled with `accent`, its label drawn in `text`. */
static void dialog__viewer_title_bar(const char *title, uint16_t accent, uint16_t text) {
    int w = display__width();
    display__fill_rect(0, 0, w, DIALOG__CHAR_H + 4, accent);
    display__set_text_color(text);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(accent);
    display__set_cursor(DIALOG__MARGIN, DIALOG__MARGIN);
    display__print(title != NULL ? title : "");
}

static void dialog__viewer_footer(const char *hint, uint16_t accent, uint16_t text) {
    int w = display__width();
    int h = display__height();
    display__fill_rect(0, h - DIALOG__CHAR_H - 4, w, DIALOG__CHAR_H + 4, accent);
    display__set_text_color(text);
    display__set_text_size(DIALOG__TEXT_SIZE);
    display__set_text_bg_color(accent);
    display__set_cursor(DIALOG__MARGIN, h - DIALOG__CHAR_H - 2);
    display__print(hint != NULL ? hint : "");
}

static bruce_result_t dialog__viewer_draw(dialog__viewer_t *viewer, bool gui) {
    if (!gui) {
        stdio__printf("--- %s ---\n", viewer->title != NULL ? viewer->title : "viewer");
        stdio__printf("%s\n", viewer->text != NULL ? viewer->text : "");
        stdio__printf("---\n");
        return BRUCE_OK;
    }

    bruce_result_t frame_result = display__begin_frame();
    if (frame_result != BRUCE_OK) {
        return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
    }
    int w = display__width();
    int h = display__height();
    int body_y = DIALOG__CHAR_H + 6;
    int usable_h = h - body_y - (DIALOG__CHAR_H + 4);
    int lines_per_screen = usable_h / (DIALOG__CHAR_H * viewer->text_size + 1);
    if (lines_per_screen < 1) { lines_per_screen = 1; }
    int max_chars = (w - 2 * DIALOG__MARGIN) / (DIALOG__CHAR_W * viewer->text_size);
    if (max_chars < 1) { max_chars = 1; }

    uint16_t pri, sec, bg, surface, text, text_muted, border, success, warning, error;
    dialog__get_colors(&pri, &sec, &bg, &surface, &text, &text_muted, &border, &success, &warning, &error);
    (void)surface;
    (void)text_muted;
    (void)border;
    (void)success;
    (void)warning;
    (void)error;
    (void)display__fill_screen(bg);
    dialog__viewer_title_bar(viewer->title, pri, text);

    display__set_text_color(text);
    display__set_text_size((uint8_t)viewer->text_size);
    display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);

    int y = body_y;
    int line = 0;
    int drawn = 0;
    const char *p = viewer->text != NULL ? viewer->text : "";
    while (*p != '\0' && drawn < lines_per_screen) {
        if (line >= viewer->scroll_y) {
            display__set_cursor(DIALOG__MARGIN, y);
            int col = 0;
            while (*p != '\0' && *p != '\n' && col < max_chars) {
                size_t bytes = (unsigned char)*p < 0x80 ? 1
                                                        : ((unsigned char)*p >= 0xF0   ? 4
                                                           : (unsigned char)*p >= 0xE0 ? 3
                                                                                       : 2);
                char ch[5] = {0};
                memcpy(ch, p, bytes);
                display__print(ch);
                col++;
                p += bytes;
            }
            y += DIALOG__CHAR_H * viewer->text_size + 1;
            drawn++;
        } else {
            while (*p != '\0' && *p != '\n') { p++; }
        }
        if (*p == '\n') { p++; }
        line++;
    }

    char footer[32];
    int total_lines = 0;
    const char *q = viewer->text != NULL ? viewer->text : "";
    while (*q != '\0') {
        if (*q == '\n') { total_lines++; }
        q++;
    }
    snprintf(footer, sizeof(footer), "%d/%d", viewer->scroll_y + 1, total_lines + 1);
    dialog__viewer_footer(footer, sec, text);
    frame_result = display__present();
    if (frame_result != BRUCE_OK) {
        return frame_result == BRUCE_ERR_NOT_FOREGROUND ? BRUCE_ERR_CANCELLED : frame_result;
    }
    return BRUCE_OK;
}

bruce_result_t
dialog__create_text_viewer(const char *title, const char *text, bruce_viewer_id_t *out_viewer) {
    if (out_viewer == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    *out_viewer = BRUCE_VIEWER_ID_INVALID;

    dialog__viewer_lock();
    if (s_viewer_mutex == NULL) { s_viewer_mutex = xSemaphoreCreateMutexStatic(&s_viewer_mutex_storage); }

    dialog__viewer_t *slot = NULL;
    for (int i = 0; i < DIALOG__VIEWER_MAX; ++i) {
        if (!s_viewers[i].used) {
            slot = &s_viewers[i];
            break;
        }
    }
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_RESOURCE_LIMIT;
    }

    slot->used = true;
    slot->owner = process__current_id();
    slot->title = title != NULL ? dialog__strdup(title) : dialog__strdup("");
    slot->text = text != NULL ? dialog__strdup(text) : dialog__strdup("");
    slot->scroll_y = 0;
    slot->text_size = DIALOG__TEXT_SIZE;
    slot->resource_id = BRUCE_RESOURCE_ID_INVALID;

    if (slot->title == NULL || slot->text == NULL) {
        dialog__viewer_free(slot);
        dialog__viewer_unlock();
        return BRUCE_ERR_NO_MEMORY;
    }

    bruce_viewer_id_t id = (bruce_viewer_id_t)(slot - s_viewers + 1);
    slot->resource_id = process_registry__resource_register(dialog__viewer_cleanup, slot);

    bool gui = dialog__current_process_wants_gui();
    dialog__note_last_call_was_gui(gui);
    dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    *out_viewer = id;
    return BRUCE_OK;
}

bruce_result_t dialog__viewer_set_text(bruce_viewer_id_t viewer, const char *text) {
    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    char *copy = text != NULL ? dialog__strdup(text) : dialog__strdup("");
    if (copy == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NO_MEMORY;
    }
    memory__free(slot->text);
    slot->text = copy;
    slot->scroll_y = 0;

    bool gui = dialog__current_process_wants_gui();
    dialog__note_last_call_was_gui(gui);
    bruce_result_t result = dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    return result;
}

bruce_result_t dialog__viewer_scroll(bruce_viewer_id_t viewer, int lines) {
    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    slot->scroll_y += lines;
    if (slot->scroll_y < 0) { slot->scroll_y = 0; }

    bool gui = dialog__current_process_wants_gui();
    dialog__note_last_call_was_gui(gui);
    bruce_result_t result = dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    return result;
}

bruce_result_t dialog__viewer_set_text_size(bruce_viewer_id_t viewer, int text_size) {
    if (text_size < 1 || text_size > 8) return BRUCE_ERR_INVALID_ARGUMENT;

    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    slot->text_size = text_size;
    bool gui = dialog__current_process_wants_gui();
    dialog__note_last_call_was_gui(gui);
    bruce_result_t result = dialog__viewer_draw(slot, gui);

    dialog__viewer_unlock();
    return result;
}

bruce_result_t dialog__viewer_close(bruce_viewer_id_t viewer) {
    dialog__viewer_lock();
    dialog__viewer_t *slot = dialog__viewer_find(viewer);
    if (slot == NULL) {
        dialog__viewer_unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (slot->owner != process__current_id()) {
        dialog__viewer_unlock();
        return BRUCE_ERR_PERMISSION;
    }

    if (slot->resource_id != BRUCE_RESOURCE_ID_INVALID) {
        process_registry__resource_release(slot->resource_id);
    }
    dialog__viewer_free(slot);
    dialog__viewer_unlock();
    return BRUCE_OK;
}
