#include "core_sdk/lvgl.h"

#include <string.h>

#include "core/display/display_font.h"
#include "core/display/display_internal.h"
#include "core/process/process_internal.h"
#include "core_sdk/config.h"
#include "core_sdk/icon.h"
#include "core_sdk/display.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/runtime.h"
#include "lvgl_assert.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "src/core/lv_global.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/themes/lv_theme_private.h"

/* Rows of the shared strip used without a framebuffer; it spans the longer
 * screen side so any viewport fits. */
#define LVGL__STRIP_ROWS 40
#define LVGL__KEY_QUEUE 16
#define LVGL__HIDDEN_WAIT_MS 100
#define LVGL__FONT_SCALES 4
#define LVGL__RADIUS 3

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
} lvgl__key_t;

/* One per process, in its process record (process__record_t.lvgl). */
typedef struct {
    lv_global_t global;
    lv_display_t *display;
    lv_indev_t *keypad;
    lv_indev_t *pointer;
    lv_draw_buf_t target; /* What LVGL renders into this frame. */
    bool redraw_all;      /* A frame could not start; draw everything next time. */
    lvgl__key_t keys[LVGL__KEY_QUEUE];
    uint8_t key_head;
    uint8_t key_count;
    uint32_t last_key;
    bool touch_down;
    lv_point_t touch;
    lv_theme_t theme;
    lv_style_t screen_style;
    lv_style_t button_style;
    lv_style_t focused_style;
    lv_style_t current_style;
    lv_style_t still_style;
} lvgl__state_t;

/* E-paper redraws slowly and visibly, so widgets change state at once there. */
static const lv_style_prop_t s_no_props[] = {0};
static const lv_style_transition_dsc_t s_no_transition = {.props = s_no_props, .time = 0};

/* Bruce's bitmap font at whole-pixel scales, shared read-only by every process. */
static lv_font_t s_fonts[LVGL__FONT_SCALES];

/* Without a framebuffer (direct render mode), every process renders through
 * this one strip in turn, then copies it into its viewport. */
static uint8_t *s_strip;
static uint32_t s_strip_size;
static StaticSemaphore_t s_strip_lock_storage;
static SemaphoreHandle_t s_strip_lock;
static portMUX_TYPE s_strip_init = portMUX_INITIALIZER_UNLOCKED;

static lvgl__state_t *lvgl__state(void) {
    process__record_t *record = process__current_record();
    return record != NULL ? record->lvgl : NULL;
}

/* LVGL's state for the calling process (LV_GLOBAL_CUSTOM). LVGL used before
 * lvgl__begin() ends the process rather than touching memory it lacks. */
lv_global_t *lv_global_default(void) {
    lvgl__state_t *state = lvgl__state();
    if (state == NULL) lvgl__assert_failed();
    return &state->global;
}

__attribute__((noreturn)) void lvgl__assert_failed(void) {
    (void)process__kill(process__current_id());
    for (;;) runtime__delay(1000);
}

/* ---- Memory: the process's own, freed with it (LV_STDLIB_CUSTOM) ---------- */

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size) { return memory__malloc(size); }

void *lv_realloc_core(void *p, size_t new_size) { return memory__realloc(p, new_size); }

void lv_free_core(void *p) { memory__free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { memset(mon_p, 0, sizeof(*mon_p)); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

/* ---- Display ---------------------------------------------------------------- */

static uint32_t lvgl__tick(void) { return (uint32_t)runtime__now(); }

/* Rendering straight into the framebuffer leaves nothing to copy; the strip
 * is copied into the viewport area by area. */
static void lvgl__flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    if (display->render_mode == LV_DISPLAY_RENDER_MODE_PARTIAL) {
        (void)display__draw_rgb_bitmap(
            (int16_t)area->x1, (int16_t)area->y1, (const uint16_t *)pixels, (int16_t)lv_area_get_width(area),
            (int16_t)lv_area_get_height(area)
        );
    }
    lv_display_flush_ready(display);
}

static bool lvgl__strip_ready(void) {
    if (s_strip_lock == NULL) {
        portENTER_CRITICAL(&s_strip_init);
        if (s_strip_lock == NULL) s_strip_lock = xSemaphoreCreateMutexStatic(&s_strip_lock_storage);
        portEXIT_CRITICAL(&s_strip_init);
    }
    if (s_strip != NULL) return true;
    int longest = display__screen_width() > display__screen_height() ? display__screen_width()
                                                                      : display__screen_height();
    uint32_t size = (uint32_t)longest * LVGL__STRIP_ROWS * sizeof(uint16_t);
    uint8_t *strip = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (strip == NULL) strip = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (strip == NULL) return false;
    s_strip_size = size;
    s_strip = strip;
    return true;
}

/* One frame: LVGL draws what was invalidated into the process's part of the
 * framebuffer, or through the shared strip without one. The frame is open the
 * whole time, which keeps the framebuffer where it is. */
static void lvgl__render(lvgl__state_t *state) {
    if (display__begin_frame() != BRUCE_OK) {
        /* E.g. BRUCE_ERR_BUSY: another process is drawing over this viewport. */
        state->redraw_all = true;
        return;
    }
    lv_display_t *display = state->display;
    uint32_t width = (uint32_t)lv_display_get_horizontal_resolution(display);
    uint32_t height = (uint32_t)lv_display_get_vertical_resolution(display);
    bruce_display_color_t *origin;
    int stride;
    if (display_internal__frame_pixels(&origin, &stride)) {
        uint32_t stride_bytes = (uint32_t)stride * sizeof(uint16_t);
        lv_draw_buf_init(&state->target, width, height, LV_COLOR_FORMAT_RGB565, stride_bytes, origin, stride_bytes * height);
        lv_display_set_draw_buffers(display, &state->target, NULL);
        lv_display_set_render_mode(display, LV_DISPLAY_RENDER_MODE_DIRECT);
        lv_display_refr_timer(NULL);
    } else if (lvgl__strip_ready()) {
        xSemaphoreTake(s_strip_lock, portMAX_DELAY);
        uint32_t row_bytes = width * sizeof(uint16_t);
        lv_draw_buf_init(&state->target, width, s_strip_size / row_bytes, LV_COLOR_FORMAT_RGB565, row_bytes, s_strip,
                         s_strip_size);
        lv_display_set_draw_buffers(display, &state->target, NULL);
        lv_display_set_render_mode(display, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_refr_timer(NULL);
        xSemaphoreGive(s_strip_lock);
    } else {
        state->redraw_all = true;
    }
    (void)display__present();
}

lv_color_t lvgl__color(uint16_t rgb565) {
    return lv_color_make((rgb565 >> 11) << 3, ((rgb565 >> 5) & 0x3F) << 2, (rgb565 & 0x1F) << 3);
}

/* ---- Bruce's font ------------------------------------------------------------ */

static int lvgl__font_scale(const lv_font_t *font) { return (int)(intptr_t)font->user_data; }

/* Each glyph sits at the top of its line, as display__draw_string() draws it. */
static bool lvgl__font_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next) {
    (void)next;
    display__glyph_t glyph;
    if (letter < 0x20 || !display_internal__active_font()->get_glyph(letter, &glyph)) return false;
    int scale = lvgl__font_scale(font);
    dsc->adv_w = (uint16_t)(glyph.advance * scale);
    dsc->box_w = (uint16_t)(glyph.width * scale);
    dsc->box_h = (uint16_t)(glyph.height * scale);
    dsc->ofs_x = 0;
    dsc->ofs_y = (int16_t)(font->line_height - dsc->box_h);
    dsc->format = LV_FONT_GLYPH_FORMAT_A1;
    dsc->is_placeholder = 0;
    dsc->gid.index = letter;
    return true;
}

/* Whole pixels scaled up, no smoothing: the look of Bruce's own text. */
static const void *lvgl__font_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *buffer) {
    display__glyph_t glyph;
    if (!display_internal__active_font()->get_glyph(dsc->gid.index, &glyph)) return NULL;
    int scale = lvgl__font_scale(dsc->resolved_font);
    uint32_t stride = lv_draw_buf_width_to_stride(dsc->box_w, LV_COLOR_FORMAT_A8);
    uint8_t *out = buffer->data;
    for (int y = 0; y < dsc->box_h; ++y) {
        for (int x = 0; x < dsc->box_w; ++x) {
            out[y * stride + x] = (glyph.columns[x / scale] >> (y / scale)) & 1u ? 0xFF : 0x00;
        }
    }
    return buffer;
}

const lv_font_t *lvgl__font(int scale) {
    if (scale < 1) scale = 1;
    if (scale > LVGL__FONT_SCALES) scale = LVGL__FONT_SCALES;
    lv_font_t *font = &s_fonts[scale - 1];
    if (font->get_glyph_dsc == NULL) {
        font->line_height = display_internal__active_font()->cell_height * scale;
        font->base_line = 0;
        font->get_glyph_bitmap = lvgl__font_bitmap;
        font->user_data = (void *)(intptr_t)scale;
        /* LVGL's symbols (LV_SYMBOL_*) live in Montserrat. */
        font->fallback = &lv_font_montserrat_14;
        font->get_glyph_dsc = lvgl__font_glyph;
    }
    return font;
}

/* ---- Bruce's icons ----------------------------------------------------------- */

typedef struct {
    lv_image_dsc_t image;
    uint8_t pixels[];
} lvgl__icon_t;

/* Drawn in the object's text color, so an icon on a button follows its focus. */
static void lvgl__icon_draw(lv_event_t *event) {
    lv_obj_t *obj = lv_event_get_current_target(event);
    lvgl__icon_t *icon = lv_event_get_user_data(event);
    lv_draw_image_dsc_t draw;
    lv_draw_image_dsc_init(&draw);
    draw.src = &icon->image;
    draw.recolor = lv_obj_get_style_text_color_filtered(obj, LV_PART_MAIN);
    draw.recolor_opa = LV_OPA_COVER;
    lv_area_t area;
    lv_obj_get_content_coords(obj, &area);
    area.x2 = area.x1 + icon->image.header.w - 1;
    area.y2 = area.y1 + icon->image.header.h - 1;
    lv_draw_image(lv_event_get_layer(event), &draw, &area);
}

static void lvgl__icon_delete(lv_event_t *event) { lv_free(lv_event_get_user_data(event)); }

lv_obj_t *lvgl__icon_create(lv_obj_t *parent, const char *name, int32_t size) {
    const bruce_icon_t *source = icon__get(name);
    if (source == NULL || size <= 0) return NULL;
    lvgl__icon_t *icon = lv_malloc(sizeof(*icon) + (size_t)(size * size));
    if (icon == NULL) return NULL;
    memset(&icon->image, 0, sizeof(icon->image));
    icon->image.header.magic = LV_IMAGE_HEADER_MAGIC;
    icon->image.header.cf = LV_COLOR_FORMAT_A8;
    icon->image.header.w = (uint32_t)size;
    icon->image.header.h = (uint32_t)size;
    icon->image.header.stride = (uint32_t)size;
    icon->image.data_size = (uint32_t)(size * size);
    icon->image.data = icon->pixels;
    uint32_t row_bytes = (source->width + 7u) / 8u;
    for (int32_t y = 0; y < size; ++y) {
        uint32_t sy = (uint32_t)(y * source->height / size);
        for (int32_t x = 0; x < size; ++x) {
            uint32_t sx = (uint32_t)(x * source->width / size);
            bool set = source->bits[sy * row_bytes + sx / 8u] & (0x80u >> (sx % 8u));
            icon->pixels[y * size + x] = set ? 0xFF : 0x00;
        }
    }
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, size, size);
    lv_obj_add_event_cb(obj, lvgl__icon_draw, LV_EVENT_DRAW_MAIN, icon);
    lv_obj_add_event_cb(obj, lvgl__icon_delete, LV_EVENT_DELETE, icon);
    return obj;
}

/* ---- Bruce's theme ----------------------------------------------------------- */

/* The default theme for widgets, with Bruce's colors, font and buttons on top:
 * flat surface buttons, filled with the primary color when focused, outlined
 * in the secondary color when checked (the current choice). Transitions
 * animate, except on e-paper. */
static void lvgl__apply_theme(lv_theme_t *theme, lv_obj_t *obj) {
    (void)theme;
    lvgl__state_t *state = lvgl__state();
    if (display__is_epaper()) {
        lv_obj_add_style(obj, &state->still_style, 0);
        lv_obj_add_style(obj, &state->still_style, LV_STATE_PRESSED);
    }
    if (lv_obj_get_parent(obj) == NULL) {
        lv_obj_add_style(obj, &state->screen_style, 0);
    } else if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_add_style(obj, &state->button_style, 0);
        lv_obj_add_style(obj, &state->current_style, LV_STATE_CHECKED);
        lv_obj_add_style(obj, &state->focused_style, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &state->focused_style, LV_STATE_FOCUS_KEY);
        lv_obj_add_style(obj, &state->focused_style, LV_STATE_PRESSED);
    }
}

static void lvgl__set_theme(lvgl__state_t *state) {
    lv_color_t background = lvgl__color(config__get_color_background());
    lv_color_t surface = lvgl__color(config__get_color_surface());
    lv_color_t text = lvgl__color(config__get_color_text());
    lv_color_t primary = lvgl__color(config__get_color_primary());
    lv_color_t secondary = lvgl__color(config__get_color_secondary());
    const lv_font_t *font = lvgl__font(1);
    lv_theme_t *base = lv_theme_default_init(
        state->display, primary, secondary, lv_color_luminance(background) < 128, font
    );

    lv_style_init(&state->screen_style);
    lv_style_set_bg_color(&state->screen_style, background);
    lv_style_set_bg_opa(&state->screen_style, LV_OPA_COVER);
    lv_style_set_text_color(&state->screen_style, text);
    lv_style_set_text_font(&state->screen_style, font);

    lv_style_init(&state->button_style);
    lv_style_set_bg_color(&state->button_style, surface);
    lv_style_set_bg_opa(&state->button_style, LV_OPA_COVER);
    lv_style_set_text_color(&state->button_style, text);
    lv_style_set_radius(&state->button_style, LVGL__RADIUS);
    lv_style_set_border_width(&state->button_style, 0);
    lv_style_set_shadow_width(&state->button_style, 0);
    lv_style_set_outline_width(&state->button_style, 0);
    lv_style_set_pad_all(&state->button_style, 2);

    lv_style_init(&state->current_style);
    lv_style_set_bg_color(&state->current_style, surface);
    lv_style_set_border_color(&state->current_style, secondary);
    lv_style_set_border_width(&state->current_style, 1);

    lv_style_init(&state->focused_style);
    lv_style_set_bg_color(&state->focused_style, primary);
    lv_style_set_text_color(&state->focused_style, background);
    lv_style_set_outline_width(&state->focused_style, 0);

    lv_style_init(&state->still_style);
    lv_style_set_transition(&state->still_style, &s_no_transition);

    state->theme = *base;
    lv_theme_set_parent(&state->theme, base);
    lv_theme_set_apply_cb(&state->theme, lvgl__apply_theme);
    lv_display_set_theme(state->display, &state->theme);
}

/* ---- Input ------------------------------------------------------------------ */

static void lvgl__push_key(lvgl__state_t *state, uint32_t key) {
    for (int i = 0; i < 2 && state->key_count < LVGL__KEY_QUEUE; ++i) {
        uint8_t slot = (uint8_t)((state->key_head + state->key_count) % LVGL__KEY_QUEUE);
        state->keys[slot] = (lvgl__key_t){.key = key, .state = i == 0 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED};
        state->key_count++;
    }
}

static void lvgl__read_keypad(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    lvgl__state_t *state = lvgl__state();
    if (state->key_count == 0) {
        data->key = state->last_key;
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    lvgl__key_t key = state->keys[state->key_head];
    state->key_head = (uint8_t)((state->key_head + 1) % LVGL__KEY_QUEUE);
    state->key_count--;
    state->last_key = key.key;
    data->key = key.key;
    data->state = key.state;
    data->continue_reading = state->key_count > 0;
}

static void lvgl__read_pointer(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    lvgl__state_t *state = lvgl__state();
    data->point = state->touch;
    data->state = state->touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Semantic keys (value != code) to LVGL's control keys; 0 when not one. */
static uint32_t lvgl__control_key(int32_t code) {
    switch (code) {
        case BRUCE_INPUT_CODE_UP: return LV_KEY_UP;
        case BRUCE_INPUT_CODE_DOWN: return LV_KEY_DOWN;
        case BRUCE_INPUT_CODE_LEFT: return LV_KEY_LEFT;
        case BRUCE_INPUT_CODE_RIGHT: return LV_KEY_RIGHT;
        case BRUCE_INPUT_CODE_SELECT: return LV_KEY_ENTER;
        case BRUCE_INPUT_CODE_BACK: return LV_KEY_ESC;
        case BRUCE_INPUT_CODE_PREV: return LV_KEY_PREV;
        case BRUCE_INPUT_CODE_NEXT: return LV_KEY_NEXT;
        case BRUCE_INPUT_CODE_HOME: return LV_KEY_HOME;
        case BRUCE_INPUT_CODE_DELETE: return LV_KEY_DEL;
        default: return 0;
    }
}

void lvgl__input(const bruce_input_event_t *event) {
    lvgl__state_t *state = lvgl__state();
    if (state == NULL || event == NULL) return;
    if (event->type == BRUCE_INPUT_TOUCH) {
        state->touch = (lv_point_t){.x = event->x, .y = event->y};
        state->touch_down = event->action != BRUCE_INPUT_RELEASE;
        return;
    }
    if (event->action != BRUCE_INPUT_PRESS) return;
    bool semantic = event->type != BRUCE_INPUT_KEY || event->value != event->code;
    uint32_t key = semantic ? lvgl__control_key(event->code) : 0;
    if (!semantic) {
        if (event->code == '\n' || event->code == '\r') key = LV_KEY_ENTER;
        else if (event->code == '\b' || event->code == 0x7F) key = LV_KEY_BACKSPACE;
        else if (event->code == 0x1B) key = LV_KEY_ESC;
        else if (event->code >= 0x20) key = (uint32_t)event->code;
    }
    if (key != 0) lvgl__push_key(state, key);
}

/* ---- Lifetime --------------------------------------------------------------- */

static void lvgl__free(process__record_t *record) {
    lvgl__state_t *state = record->lvgl;
    lv_deinit();
    record->lvgl = NULL;
    memory__free(state);
}

bruce_result_t lvgl__begin(void) {
    process__record_t *record = process__current_record();
    if (record == NULL) return BRUCE_ERR_INVALID_STATE;
    if (record->lvgl != NULL) return BRUCE_OK;
    int width = display__width(), height = display__height();
    if (width <= 0 || height <= 0) {
        width = display__screen_width();
        height = display__screen_height();
    }
    lvgl__state_t *state = memory__calloc(1, sizeof(*state));
    if (state == NULL) return BRUCE_ERR_NO_MEMORY;
    record->lvgl = state;
    lv_init();
    lv_tick_set_cb(lvgl__tick);
    state->display = lv_display_create(width, height);
    if (state->display == NULL) {
        lvgl__free(record);
        return BRUCE_ERR_NO_MEMORY;
    }
    lv_display_set_color_format(state->display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(state->display, lvgl__flush);
    /* lvgl__step() renders, inside a display frame, instead of LVGL's own
     * timer (which a change would otherwise wake to render outside one). */
    lv_display_delete_refr_timer(state->display);
    lvgl__set_theme(state);
    lv_obj_invalidate(lv_screen_active());

    lv_group_t *group = lv_group_create();
    lv_group_set_default(group);
    state->keypad = lv_indev_create();
    lv_indev_set_type(state->keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(state->keypad, lvgl__read_keypad);
    lv_indev_set_group(state->keypad, group);
    state->pointer = lv_indev_create();
    lv_indev_set_type(state->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(state->pointer, lvgl__read_pointer);
    return BRUCE_OK;
}

uint32_t lvgl__step(void) {
    lvgl__state_t *state = lvgl__state();
    if (state == NULL) return LVGL__HIDDEN_WAIT_MS;
    if (display__viewport_changed() || state->redraw_all) {
        state->redraw_all = false;
        int width = display__width(), height = display__height();
        if (width > 0 && height > 0) {
            if (width != lv_display_get_horizontal_resolution(state->display) ||
                height != lv_display_get_vertical_resolution(state->display)) {
                lv_display_set_resolution(state->display, width, height);
            }
            lv_obj_invalidate(lv_screen_active());
            lv_obj_invalidate(lv_layer_top());
        }
    }
    if (display__width() <= 0 || display__height() <= 0) return LVGL__HIDDEN_WAIT_MS;
    uint32_t wait = lv_timer_handler();
    if (state->display->inv_p > 0) lvgl__render(state);
    return wait == LV_NO_TIMER_READY ? LVGL__HIDDEN_WAIT_MS : wait;
}

void lvgl__end(void) {
    process__record_t *record = process__current_record();
    if (record == NULL || record->lvgl == NULL) return;
    lvgl__free(record);
}
