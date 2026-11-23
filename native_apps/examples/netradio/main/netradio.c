#include <stdio.h>
#include <string.h>

#include "bruce_sdk.h"
#include "core_sdk/config.h"
#include "core_sdk/lvgl.h"
#include "finder.h"
#include "stations.h"

#define RADIO_STATIONS_FILE "/sdcard/netradio/stations.m3u"
#define RADIO_DIRECTORY_URL "https://all.api.radio-browser.info"
#define RADIO_POLL_MS 100u
#define RADIO_VOLUME_STEP 10
#define RADIO_FOOTER_H 30
#define RADIO_ICON 18

typedef enum {
    RADIO_STOPPED,
    RADIO_CONNECTING, /* Getting online first. */
    RADIO_BUFFERING,  /* Asked Core to play, no sound yet. */
    RADIO_PLAYING,
    RADIO_FAILED,
} radio_play_t;

typedef enum {
    RADIO_NONE,
    RADIO_PLAY_INDEX,
    RADIO_TOGGLE,
    RADIO_SEARCH,
    RADIO_VOLUME_DOWN,
    RADIO_VOLUME_UP,
    RADIO_KEEP, /* Save or remove the current station. */
} radio_action_t;

typedef struct {
    char stations_file[128];
    char directory_url[128];
    stations_t saved;
    stations_t results;
    bool showing_results;
    char query[FINDER_QUERY_MAX];
    finder_t finder;
    station_t current;
    bool has_current;
    radio_play_t play;
    const char *failure;
    radio_action_t action;
    int arg;
    bool list_dirty;
    bool running;
    /* Widgets that change. */
    lv_obj_t *name;
    lv_obj_t *status;
    lv_obj_t *level;
    lv_obj_t *title;
    lv_obj_t *list;
    lv_obj_t *toggle;
    lv_obj_t *keep;
} radio_t;

/* ---- Playing ------------------------------------------------------------------ */

static void radio__start(radio_t *radio) {
    bruce_result_t result =
        audio__url_play(radio->current.url, radio->current.codec[0] != '\0' ? radio->current.codec : NULL);
    if (result == BRUCE_OK) {
        radio->play = RADIO_BUFFERING;
    } else {
        radio->play = RADIO_FAILED;
        radio->failure = result == BRUCE_ERR_UNSUPPORTED ? "Format not supported" : "Could not play";
    }
}

static void radio__play(radio_t *radio, const station_t *station) {
    (void)audio__file_stop();
    radio->current = *station;
    radio->has_current = true;
    if (net__is_online()) {
        radio__start(radio);
    } else if (net__connect_start() == BRUCE_OK) {
        radio->play = RADIO_CONNECTING;
    } else {
        radio->play = RADIO_FAILED;
        radio->failure = "No internet connection";
    }
}

static void radio__stop(radio_t *radio) {
    (void)audio__file_stop();
    radio->play = RADIO_STOPPED;
}

/* Follows the connection and Core's player. */
static void radio__poll_play(radio_t *radio) {
    switch (radio->play) {
        case RADIO_CONNECTING: {
            bruce_result_t result = net__connect_poll();
            if (result == BRUCE_OK) {
                radio__start(radio);
            } else if (result != BRUCE_ERR_BUSY) {
                radio->play = RADIO_FAILED;
                radio->failure = "No internet connection";
            }
            break;
        }
        case RADIO_BUFFERING:
            if (!audio__file_is_open()) {
                radio->play = RADIO_FAILED;
                radio->failure = "Could not play";
            } else if (audio__file_position_ms() > 0) {
                radio->play = RADIO_PLAYING;
            }
            break;
        case RADIO_PLAYING:
            if (!audio__file_is_open()) {
                radio->play = RADIO_FAILED;
                radio->failure = "Stream ended";
            }
            break;
        default: break;
    }
}

static int radio__saved_index(const radio_t *radio) {
    for (size_t i = 0; i < radio->saved.count; ++i) {
        if (strcmp(radio->saved.items[i].url, radio->current.url) == 0) return (int)i;
    }
    return -1;
}

/* ---- Widgets ----------------------------------------------------------------- */

static void radio__clicked(lv_event_t *event) {
    radio_t *radio = lv_event_get_user_data(event);
    int32_t packed = (int32_t)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    radio->action = (radio_action_t)(packed >> 16);
    radio->arg = packed & 0xFFFF;
}

static void radio__on_click(radio_t *radio, lv_obj_t *obj, radio_action_t action, int arg) {
    lv_obj_set_user_data(obj, (void *)(intptr_t)(((int32_t)action << 16) | (arg & 0xFFFF)));
    lv_obj_add_event_cb(obj, radio__clicked, LV_EVENT_CLICKED, radio);
}

static lv_obj_t *radio__box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_style_pad_gap(box, 3, 0);
    return box;
}

static lv_obj_t *radio__muted(lv_obj_t *parent, const char *text) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lvgl__color(config__get_color_text_muted()), 0);
    return label;
}

/* A footer button holding one or two icons (the second hidden at first). */
static lv_obj_t *radio__icon_button(radio_t *radio, lv_obj_t *parent, const char *icon, const char *alternate,
                                    radio_action_t action) {
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_height(button, LV_PCT(100));
    lv_obj_set_flex_grow(button, 1);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lvgl__icon_create(button, icon, RADIO_ICON);
    if (alternate != NULL) {
        lv_obj_t *second = lvgl__icon_create(button, alternate, RADIO_ICON);
        if (second != NULL) lv_obj_add_flag(second, LV_OBJ_FLAG_HIDDEN);
    }
    radio__on_click(radio, button, action, 0);
    return button;
}

/* Shows a two-icon button's first icon when `first`, else its second. */
static void radio__show_icon(lv_obj_t *button, bool first) {
    lv_obj_t *a = lv_obj_get_child(button, 0);
    lv_obj_t *b = lv_obj_get_child(button, 1);
    if (a == NULL || b == NULL || lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN) == !first) return;
    if (first) {
        lv_obj_remove_flag(a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
    }
}

static void radio__build(radio_t *radio) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 4, 0);
    lv_obj_set_style_pad_gap(screen, 3, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    radio->name = lv_label_create(screen);
    lv_obj_set_width(radio->name, LV_PCT(100));
    lv_label_set_long_mode(radio->name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(radio->name, lvgl__font(2), 0);
    lv_obj_set_style_text_color(radio->name, lvgl__color(config__get_color_primary()), 0);
    lv_label_set_text(radio->name, "Internet Radio");
    radio->status = radio__muted(screen, "");
    lv_obj_set_width(radio->status, LV_PCT(100));
    lv_label_set_long_mode(radio->status, LV_LABEL_LONG_DOT);
    radio->level = lv_bar_create(screen);
    lv_obj_set_size(radio->level, LV_PCT(100), 4);
    lv_bar_set_range(radio->level, 0, 100);

    radio->title = radio__muted(screen, "");
    radio->list = radio__box(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(radio->list, LV_PCT(100));
    lv_obj_set_flex_grow(radio->list, 1);
    lv_obj_add_flag(radio->list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *footer = radio__box(screen, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(footer, LV_PCT(100), RADIO_FOOTER_H);
    radio->toggle = radio__icon_button(radio, footer, "play", "pause", RADIO_TOGGLE);
    radio__icon_button(radio, footer, "magnify", NULL, RADIO_SEARCH);
    radio__icon_button(radio, footer, "volume-low", NULL, RADIO_VOLUME_DOWN);
    radio__icon_button(radio, footer, "volume-high", NULL, RADIO_VOLUME_UP);
    radio->keep = radio__icon_button(radio, footer, "plus", "delete", RADIO_KEEP);
    radio->list_dirty = true;
}

static void radio__fill_list(radio_t *radio) {
    const stations_t *stations = radio->showing_results ? &radio->results : &radio->saved;
    lv_obj_clean(radio->list);
    for (size_t i = 0; i < stations->count; ++i) {
        lv_obj_t *button = lv_button_create(radio->list);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_label_set_text(label, stations->items[i].name);
        radio__on_click(radio, button, RADIO_PLAY_INDEX, (int)i);
    }
    if (stations->count == 0) {
        radio__muted(radio->list, radio->showing_results ? "No stations found" : "No stations: search to add some");
    }
    char title[FINDER_QUERY_MAX + 16];
    if (radio->showing_results) snprintf(title, sizeof(title), "Found for \"%s\"", radio->query);
    else snprintf(title, sizeof(title), "Stations");
    lv_label_set_text(radio->title, title);
    radio->list_dirty = false;
}

static void radio__set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void radio__update(radio_t *radio) {
    if (radio->list_dirty) radio__fill_list(radio);
    if (radio->has_current) radio__set_text(radio->name, radio->current.name);
    char status[64];
    const char *state = radio->play == RADIO_CONNECTING  ? "Connecting..."
                        : radio->play == RADIO_BUFFERING ? "Buffering..."
                        : radio->play == RADIO_PLAYING   ? "Playing"
                        : radio->play == RADIO_FAILED    ? radio->failure
                        : radio->finder.state != FINDER_IDLE ? "Searching..."
                                                              : "Stopped";
    snprintf(status, sizeof(status), "%s  Vol %d%%", state, config__get_sound_volume());
    radio__set_text(radio->status, status);
    int level = radio->play == RADIO_PLAYING ? audio__file_level() : 0;
    if (lv_bar_get_value(radio->level) != level) lv_bar_set_value(radio->level, level, LV_ANIM_OFF);
    bool active = radio->play == RADIO_CONNECTING || radio->play == RADIO_BUFFERING || radio->play == RADIO_PLAYING;
    radio__show_icon(radio->toggle, !active);
    radio__show_icon(radio->keep, !radio->has_current || radio__saved_index(radio) < 0);
}

/* ---- Actions ------------------------------------------------------------------ */

static void radio__search(radio_t *radio) {
    char query[FINDER_QUERY_MAX] = "";
    bruce_result_t asked = dialog__text_input("Search stations", "Name", radio->query, false, query, sizeof(query));
    lv_obj_invalidate(lv_screen_active());
    if (asked != BRUCE_OK || query[0] == '\0') return;
    snprintf(radio->query, sizeof(radio->query), "%s", query);
    if (finder__start(&radio->finder, radio->directory_url, query) != BRUCE_OK) {
        (void)dialog__message(BRUCE_DIALOG_WARNING, "Internet Radio", "No internet connection");
        lv_obj_invalidate(lv_screen_active());
    }
}

static void radio__poll_search(radio_t *radio) {
    if (radio->finder.state == FINDER_IDLE) return;
    bruce_result_t result = finder__poll(&radio->finder, &radio->results);
    if (result == BRUCE_ERR_BUSY) return;
    if (result == BRUCE_OK) {
        radio->showing_results = true;
        radio->list_dirty = true;
        return;
    }
    (void)dialog__message(BRUCE_DIALOG_WARNING, "Internet Radio",
                          net__is_online() ? "Search failed, try again later" : "No internet connection");
    lv_obj_invalidate(lv_screen_active());
}

/* Saves the current station, or removes it when it is already saved. */
static void radio__keep(radio_t *radio) {
    if (!radio->has_current) return;
    int index = radio__saved_index(radio);
    if (index >= 0) stations__remove(&radio->saved, (size_t)index);
    else if (!stations__add(&radio->saved, &radio->current)) return;
    if (stations__save(radio->stations_file, &radio->saved) != BRUCE_OK) {
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Internet Radio", "Could not save the station list");
        lv_obj_invalidate(lv_screen_active());
    }
    if (!radio->showing_results) radio->list_dirty = true;
}

static void radio__volume(int step) {
    int volume = config__get_sound_volume() + step;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    (void)config__set_sound_volume(volume);
}

static void radio__run_action(radio_t *radio) {
    radio_action_t action = radio->action;
    radio->action = RADIO_NONE;
    const stations_t *stations = radio->showing_results ? &radio->results : &radio->saved;
    switch (action) {
        case RADIO_PLAY_INDEX:
            if ((size_t)radio->arg < stations->count) radio__play(radio, &stations->items[radio->arg]);
            break;
        case RADIO_TOGGLE:
            if (radio->play == RADIO_CONNECTING || radio->play == RADIO_BUFFERING || radio->play == RADIO_PLAYING) {
                radio__stop(radio);
            } else if (radio->has_current) {
                radio__play(radio, &radio->current);
            } else if (stations->count > 0) {
                radio__play(radio, &stations->items[0]);
            }
            break;
        case RADIO_SEARCH: radio__search(radio); break;
        case RADIO_VOLUME_DOWN: radio__volume(-RADIO_VOLUME_STEP); break;
        case RADIO_VOLUME_UP: radio__volume(RADIO_VOLUME_STEP); break;
        case RADIO_KEEP: radio__keep(radio); break;
        default: break;
    }
}

/* Back leaves search results, then the app. */
static void radio__on_input(radio_t *radio, const bruce_input_event_t *event) {
    bool back = event->type != BRUCE_INPUT_TOUCH && event->action == BRUCE_INPUT_PRESS &&
                event->code == BRUCE_INPUT_CODE_BACK;
    if (!back) {
        lvgl__input(event);
    } else if (radio->showing_results) {
        radio->showing_results = false;
        radio->list_dirty = true;
    } else {
        radio->running = false;
    }
}

int app_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    radio_t *radio = memory__calloc(1, sizeof(*radio));
    if (radio == NULL) return BRUCE_ERR_NO_MEMORY;
    app_config__get_string(NULL, "stations.file", RADIO_STATIONS_FILE, radio->stations_file,
                           sizeof(radio->stations_file));
    app_config__get_string(NULL, "directory.url", RADIO_DIRECTORY_URL, radio->directory_url,
                           sizeof(radio->directory_url));
    if (stations__load(radio->stations_file, &radio->saved) != BRUCE_OK) stations__defaults(&radio->saved);

    bruce_result_t started = lvgl__begin();
    if (started != BRUCE_OK) {
        memory__free(radio);
        return started;
    }
    radio__build(radio);
    radio->running = true;
    while (radio->running) {
        radio__poll_play(radio);
        radio__poll_search(radio);
        radio__update(radio);
        uint32_t wait = lvgl__step();
        if (radio->action != RADIO_NONE) {
            radio__run_action(radio);
            continue;
        }
        bruce_input_event_t event;
        bruce_result_t result = input__read(&event, wait < RADIO_POLL_MS ? wait : RADIO_POLL_MS);
        if (result == BRUCE_OK) radio__on_input(radio, &event);
        else if (result == BRUCE_ERR_NOT_FOREGROUND) (void)runtime__delay(RADIO_POLL_MS);
        else if (result != BRUCE_ERR_TIMEOUT) break;
    }
    finder__cancel(&radio->finder);
    radio__stop(radio);
    lvgl__end();
    memory__free(radio);
    return BRUCE_OK;
}
