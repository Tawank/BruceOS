#include "player_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "args.h"
#include "core_sdk/audio.h"
#include "core_sdk/config.h"
#include "core_sdk/dialog.h"
#include "core_sdk/display.h"
#include "core_sdk/environment.h"
#include "core_sdk/input.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* Simple GUI music player: plays one .wav/.mp3 file at a time through
 * core/audio's audio__file_play() and steps through the other .wav/.mp3
 * files in the same folder for Prev/Next, like a typical single-folder
 * music player rather than a real playlist manager. */

#define PLAYER_APP__SEEK_STEP_MS 5000u
#define PLAYER_APP__LONG_PRESS_MS 500u
#define PLAYER_APP__VOLUME_STEP 5
#define PLAYER_APP__BAR_COUNT 12
#define PLAYER_APP__BAR_WOBBLE_PERIOD_TICKS 20u /* ~20 * input__wait's 80ms poll below */

/* Controls PREV/NEXT cycle focus through; SELECT activates whichever is
 * focused. Everything before CLOSE sits in the bottom row; CLOSE is the X in
 * the top-right corner. Play/pause starts focused. */
typedef enum {
    PLAYER_APP__BUTTON_VOLUME,
    PLAYER_APP__BUTTON_PREV,
    PLAYER_APP__BUTTON_PLAY_PAUSE,
    PLAYER_APP__BUTTON_NEXT,
    PLAYER_APP__BUTTON_MINIMIZE,
    PLAYER_APP__BUTTON_CLOSE,
    PLAYER_APP__BUTTON_COUNT,
} player__button_t;

#define PLAYER_APP__BOTTOM_BUTTONS PLAYER_APP__BUTTON_CLOSE

static const uint16_t PLAYER_APP__BAR_COLORS[] = {
    BRUCE_COLOR_RED,    BRUCE_COLOR_ORANGE, BRUCE_COLOR_YELLOW, BRUCE_COLOR_GREEN,
    BRUCE_COLOR_CYAN,   BRUCE_COLOR_BLUE,   BRUCE_COLOR_MAGENTA, BRUCE_COLOR_PURPLE,
};

typedef struct {
    char (*paths)[BRUCE_STORAGE_PATH_MAX];
    size_t count;
    size_t current;
} player__playlist_t;

static bool player__is_supported_path(const char *path) {
    if (path == NULL) return false;
    const char *extension = strrchr(path, '.');
    return extension != NULL && (strcasecmp(extension, ".wav") == 0 || strcasecmp(extension, ".mp3") == 0);
}

/* Resolves `path` against $PWD the same way the shell's own builtins do,
 * since this can be run as a plain command with a relative argument (e.g.
 * "player song.mp3") with no path-resolution help from the caller.
 * Duplicated in miniature rather than shared with bnu's own
 * bnu__resolve_path() or ffplay_app.c's identical helper, same reasoning as
 * ffplay_app.c gives for its own copy: this module intentionally doesn't
 * depend on either. */
static bool player__resolve_path(const char *path, char *out_path) {
    char combined[BRUCE_STORAGE_PATH_MAX * 2];
    const char *pwd = environment__get("PWD");
    if (pwd == NULL || pwd[0] != '/') pwd = "/";
    if (path == NULL || path[0] == '\0') path = pwd;
    int written = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path)
                                  : snprintf(
                                        combined, sizeof(combined), "%s%s%s", pwd,
                                        strcmp(pwd, "/") == 0 ? "" : "/", path
                                    );
    if (written < 0 || (size_t)written >= sizeof(combined)) return false;

    size_t out_length = 1;
    out_path[0] = '/';
    out_path[1] = '\0';
    const char *cursor = combined;
    while (*cursor != '\0') {
        while (*cursor == '/') cursor++;
        const char *component = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        size_t length = (size_t)(cursor - component);
        if (length == 0 || (length == 1 && component[0] == '.')) continue;
        if (length == 2 && component[0] == '.' && component[1] == '.') {
            while (out_length > 1 && out_path[out_length - 1] != '/') out_length--;
            if (out_length > 1) out_length--;
            out_path[out_length] = '\0';
            continue;
        }
        size_t separator = out_length > 1 ? 1u : 0u;
        if (out_length + separator + length >= BRUCE_STORAGE_PATH_MAX) return false;
        if (separator != 0) out_path[out_length++] = '/';
        memcpy(out_path + out_length, component, length);
        out_length += length;
        out_path[out_length] = '\0';
    }
    return true;
}

static int player__compare_paths(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

/* Lists `opened_path`'s own directory and keeps the other .wav/.mp3 entries
 * (sorted by name) as the Prev/Next playlist, with `current` pointing at
 * opened_path itself. Leaves *out all-zero (an empty playlist) on any
 * failure -- player_app_main() falls back to a one-track playlist of just
 * opened_path in that case, so playback still works, just without Prev/Next. */
static void player__build_playlist(const char *opened_path, player__playlist_t *out) {
    out->paths = NULL;
    out->count = 0;
    out->current = 0;

    char directory[BRUCE_STORAGE_PATH_MAX];
    const char *slash = strrchr(opened_path, '/');
    size_t dir_length = slash != NULL ? (size_t)(slash - opened_path) : 0;
    if (dir_length == 0) {
        directory[0] = '/';
        directory[1] = '\0';
    } else if (dir_length < sizeof(directory)) {
        memcpy(directory, opened_path, dir_length);
        directory[dir_length] = '\0';
    } else {
        return;
    }

    size_t total = 0;
    if (storage__list(directory, NULL, 0, &total) != BRUCE_OK || total == 0) return;
    bruce_storage_entry_t *entries = memory__malloc(total * sizeof(*entries));
    if (entries == NULL) return;
    if (storage__list(directory, entries, total, &total) != BRUCE_OK) {
        memory__free(entries);
        return;
    }

    char (*paths)[BRUCE_STORAGE_PATH_MAX] = memory__malloc(total * sizeof(*paths));
    if (paths == NULL) {
        memory__free(entries);
        return;
    }
    size_t count = 0;
    for (size_t i = 0; i < total; ++i) {
        if (entries[i].type != BRUCE_STORAGE_ENTRY_FILE || !player__is_supported_path(entries[i].name)) continue;
        bool is_root = strcmp(directory, "/") == 0;
        int written = snprintf(paths[count], sizeof(paths[count]), "%s%s%s", directory, is_root ? "" : "/", entries[i].name);
        if (written > 0 && (size_t)written < sizeof(paths[count])) count++;
    }
    memory__free(entries);
    if (count == 0) {
        memory__free(paths);
        return;
    }

    qsort(paths, count, sizeof(*paths), player__compare_paths);
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(paths[i], opened_path) == 0) out->current = i;
    }
    out->paths = paths;
    out->count = count;
}

static void player__free_playlist(player__playlist_t *playlist) {
    if (playlist->paths != NULL) memory__free(playlist->paths);
    playlist->paths = NULL;
    playlist->count = 0;
}

/* Stops whatever is currently playing and starts playlist->paths[current],
 * waiting out the old track's teardown first -- audio__file_play() returns
 * BRUCE_ERR_BUSY while another file is still open, and audio__file_stop()
 * only *requests* a stop that the background player notices on its own next
 * poll, so this can't just call stop() then play() back to back. */
static bruce_result_t player__play_track(player__playlist_t *playlist, char *out_title, size_t out_title_size) {
    (void)audio__file_stop();
    while (audio__file_is_open()) {
        if (runtime__sleep(10u) != BRUCE_OK) break;
    }
    const char *path = playlist->paths[playlist->current];
    bruce_result_t result = audio__file_play(path);
    if (result == BRUCE_OK) {
        const char *base_name = strrchr(path, '/');
        snprintf(out_title, out_title_size, "%s", base_name != NULL ? base_name + 1 : path);
    }
    return result;
}

static void player__go_prev(player__playlist_t *playlist, char *title, size_t title_size, bool *paused) {
    if (playlist->count <= 1) return;
    playlist->current = (playlist->current + playlist->count - 1) % playlist->count;
    if (player__play_track(playlist, title, title_size) == BRUCE_OK) *paused = false;
}

static void player__go_next(player__playlist_t *playlist, char *title, size_t title_size, bool *paused) {
    if (playlist->count <= 1) return;
    playlist->current = (playlist->current + 1) % playlist->count;
    if (player__play_track(playlist, title, title_size) == BRUCE_OK) *paused = false;
}

static void player__move_focus(int *focused, int delta) {
    *focused = (*focused + delta + PLAYER_APP__BUTTON_COUNT) % PLAYER_APP__BUTTON_COUNT;
}

static void player__change_volume(int delta) {
    int volume = config__get_sound_volume() + delta;
    (void)config__set_sound_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
}

/* Returns true when the player should close. */
static bool player__activate_focused(
    int focused, player__playlist_t *playlist, char *title, size_t title_size, bool *paused, bool *volume_open
) {
    switch ((player__button_t)focused) {
        case PLAYER_APP__BUTTON_PREV: player__go_prev(playlist, title, title_size, paused); break;
        case PLAYER_APP__BUTTON_PLAY_PAUSE:
            *paused = !*paused;
            (void)(*paused ? audio__file_pause() : audio__file_resume());
            break;
        case PLAYER_APP__BUTTON_NEXT: player__go_next(playlist, title, title_size, paused); break;
        case PLAYER_APP__BUTTON_VOLUME: *volume_open = true; break;
        case PLAYER_APP__BUTTON_MINIMIZE: (void)process__to_background(); break;
        case PLAYER_APP__BUTTON_CLOSE: return true;
        default: break;
    }
    return false;
}

static void player__seek_backward(void) {
    uint32_t position = audio__file_position_ms();
    (void)audio__file_seek(position > PLAYER_APP__SEEK_STEP_MS ? position - PLAYER_APP__SEEK_STEP_MS : 0);
}

static void player__seek_forward(void) { (void)audio__file_seek(audio__file_position_ms() + PLAYER_APP__SEEK_STEP_MS); }

static void player__format_time(uint32_t milliseconds, char *out, size_t size) {
    uint32_t total_seconds = milliseconds / 1000u;
    /* % 100u clamps minutes (otherwise unbounded) to a provable 2 digits --
     * silences -Wformat-truncation, and no real audio file runs past
     * 99 minutes anyway. */
    unsigned int minutes = (unsigned int)((total_seconds / 60u) % 100u);
    unsigned int seconds = (unsigned int)(total_seconds % 60u);
    snprintf(out, size, "%02u:%02u", minutes, seconds);
}

/* Paused shows a play triangle (pressing SELECT will play) and playing
 * shows two pause bars (pressing SELECT will pause) -- the icon always
 * depicts what the next press does, the usual media-player convention. */
static void player__draw_play_pause_icon(int16_t cx, int16_t cy, int16_t half, bool paused, uint16_t color) {
    if (paused) {
        (void)display__fill_triangle(cx - half, cy - half, cx - half, cy + half, cx + half, cy, color);
    } else {
        int16_t bar_width = half / 2 > 0 ? half / 2 : 1;
        (void)display__fill_rect(cx - half, cy - half, bar_width, half * 2, color);
        (void)display__fill_rect(cx + half - bar_width, cy - half, bar_width, half * 2, color);
    }
}

/* A triangle plus a trailing bar, the usual "skip" glyph, mirrored for
 * Prev vs Next rather than computed from a signed direction -- clearer to
 * read than the equivalent mirrored arithmetic. */
static void player__draw_skip_icon(int16_t cx, int16_t cy, int16_t half, bool next, uint16_t color) {
    int16_t bar_width = half / 2 > 0 ? half / 2 : 1;
    if (next) {
        (void)display__fill_triangle(cx - half, cy - half, cx - half, cy + half, cx + half - bar_width, cy, color);
        (void)display__fill_rect(cx + half - bar_width, cy - half, bar_width, half * 2, color);
    } else {
        (void)display__fill_triangle(cx + half, cy - half, cx + half, cy + half, cx - half + bar_width, cy, color);
        (void)display__fill_rect(cx - half, cy - half, bar_width, half * 2, color);
    }
}

/* A single bar along the bottom edge -- the universal "minimize window" glyph. */
static void player__draw_minimize_icon(int16_t cx, int16_t cy, int16_t half, uint16_t color) {
    int16_t bar_height = half / 3 > 0 ? half / 3 : 1;
    (void)display__fill_rect(cx - half, cy + half - bar_height, half * 2, bar_height, color);
}

static void player__draw_close_icon(int16_t cx, int16_t cy, int16_t half, uint16_t color) {
    for (int16_t offset = -1; offset <= 1; ++offset) {
        (void)display__draw_line(cx - half + offset, cy - half, cx + half + offset, cy + half, color);
        (void)display__draw_line(cx - half + offset, cy + half, cx + half + offset, cy - half, color);
    }
}

/* The X in the top-right corner, boxed while focused. */
static void player__draw_close(int width, int margin, int char_height, bool focused) {
    const int16_t half = 5;
    int16_t cx = (int16_t)(width - margin - half - 3);
    int16_t cy = (int16_t)(margin + char_height / 2);
    uint16_t color = config__get_color_primary();
    if (focused) {
        int16_t box = (int16_t)(half * 2 + 7);
        (void)display__fill_rect((int16_t)(cx - half - 3), (int16_t)(cy - half - 3), box, box, color);
        color = config__get_color_background();
    }
    player__draw_close_icon(cx, cy, half, color);
}

/* Box plus cone, with the cone's mouth at cx + half. */
static void player__draw_speaker_icon(int16_t cx, int16_t cy, int16_t half, uint16_t color) {
    int16_t box = half * 2 / 3 > 0 ? half * 2 / 3 : 1;
    (void)display__fill_rect(cx - half, cy - box / 2, box, box, color);
    (void)display__fill_triangle(cx - half + box - 1, cy, cx + half, cy - half, cx + half, cy + half, color);
}

/* Speaker with the current volume written beside it. */
static void player__draw_volume_button(int16_t cx, int16_t cy, int16_t char_width, uint16_t color) {
    char text[8];
    snprintf(text, sizeof(text), "%d%%", config__get_sound_volume());
    const int16_t icon_half = 6;
    int16_t content = (int16_t)(icon_half * 2 + 3 + (int)strlen(text) * char_width);
    int16_t left = (int16_t)(cx - content / 2);
    player__draw_speaker_icon((int16_t)(left + icon_half), cy, icon_half, color);
    (void)display__set_text_color(color);
    (void)display__draw_string(text, (int16_t)(left + icon_half * 2 + 3), (int16_t)(cy - 4));
}

static void player__draw_volume_bar(int width, int cy, int margin, int16_t char_width, uint16_t color) {
    char text[8];
    int volume = config__get_sound_volume();
    snprintf(text, sizeof(text), "%d%%", volume);
    const int16_t icon_half = 8;
    player__draw_speaker_icon((int16_t)(margin + icon_half), (int16_t)cy, icon_half, color);
    int text_w = 4 * char_width;
    int track_x = margin + icon_half * 2 + 8;
    int track_w = width - margin - text_w - 6 - track_x;
    const int track_h = 8;
    (void)display__draw_rect((int16_t)track_x, (int16_t)(cy - track_h / 2), (int16_t)track_w, track_h, color);
    int fill = (track_w - 2) * volume / 100;
    if (fill > 0) {
        int16_t fill_y = (int16_t)(cy - track_h / 2 + 1);
        (void)display__fill_rect((int16_t)(track_x + 1), fill_y, (int16_t)fill, track_h - 2, color);
    }
    (void)display__set_text_color(color);
    (void)display__draw_right_string(text, (int16_t)(width - margin), (int16_t)(cy - 4));
}

static bruce_result_t
player__draw(const char *title, player__playlist_t *playlist, bool paused, int focused, bool volume_open) {
    int width = display__width();
    int height = display__height();
    uint16_t primary = config__get_color_primary();
    uint16_t secondary = config__get_color_secondary();
    uint16_t background = config__get_color_background();

    bruce_result_t result = display__begin_frame();
    if (result != BRUCE_OK) return result;
    (void)display__fill_screen(background);
    (void)display__set_text_bg_color(BRUCE_COLOR_TRANSPARENT);
    (void)display__set_text_size(1);

    int16_t char_width = 6, char_height = 8;
    if (display__get_font_metrics(&char_width, &char_height) != BRUCE_OK || char_height <= 0) char_height = 8;

    /* A quarter-screen preview tile only shows the title, bars, seek bar and time. */
    bool compact = width * height * 4 < display__screen_width() * display__screen_height();
    const int margin = compact ? 3 : 6;
    const int gap = compact ? 3 : 10;
    const int icon_half = 10;
    const int highlight_half = icon_half + 5;
    const int progress_height = compact ? 4 : 6;
    int controls_y = height - margin - highlight_half;
    int time_row_y = compact ? height - margin - char_height : controls_y - highlight_half - 4 - char_height;
    int progress_y = time_row_y - 2 - progress_height;

    (void)display__set_text_color(secondary);
    (void)display__draw_centre_string(title, width / 2, margin);
    if (!compact) player__draw_close(width, margin, char_height, focused == PLAYER_APP__BUTTON_CLOSE);


    /* Reactive color bars: each bar's height is audio__file_level() (the
     * real decoded playback level, 0-100) scaled by a per-bar wobble that
     * cycles over time, so the row visibly pulses with the music instead of
     * every bar moving in lockstep. Flat when paused/silent, since the
     * level itself is then 0. */
    int bars_top = margin + char_height + (compact ? 3 : 6);
    int bars_bottom = progress_y - gap;
    if (bars_bottom < bars_top) bars_bottom = bars_top;
    int bars_height = bars_bottom - bars_top;
    uint8_t level = audio__file_level();
    uint32_t tick = (uint32_t)(runtime__now() / 80u);
    int bar_gap = 4;
    int bars_total_width = width - margin * 2;
    int bar_width = (bars_total_width - bar_gap * (PLAYER_APP__BAR_COUNT - 1)) / PLAYER_APP__BAR_COUNT;
    if (bar_width < 2) bar_width = 2;
    for (int i = 0; i < PLAYER_APP__BAR_COUNT; ++i) {
        uint32_t phase = (tick + (uint32_t)i * 3u) % PLAYER_APP__BAR_WOBBLE_PERIOD_TICKS;
        uint32_t half_period = PLAYER_APP__BAR_WOBBLE_PERIOD_TICKS / 2u;
        uint32_t wobble = phase < half_period ? phase : PLAYER_APP__BAR_WOBBLE_PERIOD_TICKS - phase;
        uint32_t height_percent = ((uint32_t)level * (half_period / 2u + wobble)) / half_period;
        if (height_percent > 100u) height_percent = 100u;
        int bar_height = (int)(((uint32_t)bars_height * height_percent) / 100u);
        int x = margin + i * (bar_width + bar_gap);
        uint16_t color = PLAYER_APP__BAR_COLORS[(size_t)i % (sizeof(PLAYER_APP__BAR_COLORS) / sizeof(PLAYER_APP__BAR_COLORS[0]))];
        (void)display__fill_rect((int16_t)x, (int16_t)(bars_bottom - bar_height), (int16_t)bar_width, (int16_t)bar_height, color);
    }

    /* Progress bar. */
    (void)display__draw_rect((int16_t)margin, (int16_t)progress_y, (int16_t)(width - margin * 2), (int16_t)progress_height, secondary);
    uint32_t position_ms = audio__file_position_ms();
    uint32_t duration_ms = audio__file_duration_ms();
    int fillable_width = width - margin * 2 - 2;
    int fill_width = duration_ms > 0 ? (int)(((uint64_t)fillable_width * position_ms) / duration_ms) : 0;
    if (fill_width > fillable_width) fill_width = fillable_width;
    if (fill_width > 0) {
        (void)display__fill_rect((int16_t)(margin + 1), (int16_t)(progress_y + 1), (int16_t)fill_width, (int16_t)(progress_height - 2), primary);
    }
    char position_text[8], duration_text[8];
    player__format_time(position_ms, position_text, sizeof(position_text));
    player__format_time(duration_ms, duration_text, sizeof(duration_text));
    (void)display__set_text_color(secondary);
    (void)display__draw_string(position_text, (int16_t)margin, (int16_t)time_row_y);
    (void)display__draw_right_string(duration_text, (int16_t)(width - margin), (int16_t)time_row_y);

    if (compact) return display__present();
    if (volume_open) {
        player__draw_volume_bar(width, controls_y, margin, char_width, primary);
        return display__present();
    }

    /* Bottom controls. PREV/NEXT move `focused` among these instead of acting
     * directly, so SELECT can activate whichever one is highlighted -- a
     * focused control draws on top of a filled box in the inverse of its own
     * color, same convention as a selected row in a list dialog. */
    uint16_t has_siblings_color = playlist->count > 1 ? primary : secondary;
    int cell_w = width / PLAYER_APP__BOTTOM_BUTTONS;
    for (int i = 0; i < PLAYER_APP__BOTTOM_BUTTONS; ++i) {
        int16_t x = (int16_t)(cell_w * i + cell_w / 2);
        bool skip = i == PLAYER_APP__BUTTON_PREV || i == PLAYER_APP__BUTTON_NEXT;
        uint16_t color = skip ? has_siblings_color : primary;
        if (i == focused) {
            int box_half = i == PLAYER_APP__BUTTON_VOLUME ? cell_w / 2 - 2 : highlight_half;
            (void)display__fill_rect(
                (int16_t)(x - box_half), (int16_t)(controls_y - highlight_half), (int16_t)(box_half * 2),
                (int16_t)(highlight_half * 2), primary
            );
            color = background;
        }
        int16_t y = (int16_t)controls_y;
        switch ((player__button_t)i) {
            case PLAYER_APP__BUTTON_PREV: player__draw_skip_icon(x, y, icon_half, false, color); break;
            case PLAYER_APP__BUTTON_PLAY_PAUSE:
                player__draw_play_pause_icon(x, y, icon_half, paused, color);
                break;
            case PLAYER_APP__BUTTON_NEXT: player__draw_skip_icon(x, y, icon_half, true, color); break;
            case PLAYER_APP__BUTTON_VOLUME: player__draw_volume_button(x, y, char_width, color); break;
            case PLAYER_APP__BUTTON_MINIMIZE: player__draw_minimize_icon(x, y, icon_half, color); break;
            default: break;
        }
    }

    return display__present();
}

int player_app_main(int argc, char **argv) {
    ArgParser *parser = ap_new_parser();
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(parser, "Play a local .wav or .mp3 file with basic playback controls.");
    ap_add_optional_arg(parser, "path", "Path to a .wav or .mp3 file");
    ap_unknown_options_as_args(parser);
    ap_allow_extra_args(parser);
    ap_first_pos_arg_ends_option_parsing(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) {
        ap_status_t status = ap_get_status(parser);
        ap_free(parser);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }
    const char *arg_path = ap_get_arg(parser, "path");
    ap_free(parser);

    char path[BRUCE_STORAGE_PATH_MAX];
    if (arg_path != NULL) {
        if (!player__resolve_path(arg_path, path)) {
            stdio__printf("player: %s: path too long\n", arg_path);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
    } else if (dialog__pick_file("/", NULL, path, sizeof(path), "Player") != BRUCE_OK) {
        return BRUCE_OK; /* cancelled */
    }
    if (!player__is_supported_path(path)) {
        (void)dialog__message(BRUCE_DIALOG_ERROR, "Player", "Only .wav and .mp3 files are supported.");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    player__playlist_t playlist;
    player__build_playlist(path, &playlist);
    if (playlist.count == 0) {
        /* No usable sibling listing (storage error, or this really is the
         * only track in its folder) -- fall back to a one-track playlist of
         * just the file we were asked to open, so playback still works and
         * Prev/Next simply become no-ops below. */
        char (*solo)[BRUCE_STORAGE_PATH_MAX] = memory__malloc(sizeof(*solo));
        if (solo == NULL) return BRUCE_ERR_NO_MEMORY;
        snprintf(solo[0], sizeof(solo[0]), "%s", path);
        playlist.paths = solo;
        playlist.count = 1;
        playlist.current = 0;
    }

    char title[BRUCE_STORAGE_NAME_MAX];
    bruce_result_t result = player__play_track(&playlist, title, sizeof(title));
    if (result != BRUCE_OK) {
        player__free_playlist(&playlist);
        (void)dialog__message(
            BRUCE_DIALOG_ERROR, "Player",
            result == BRUCE_ERR_UNSUPPORTED ? "Unsupported file (only 8/16-bit PCM .wav or .mp3)."
                                             : "Could not play this file."
        );
        return result;
    }

    (void)input__flush();
    bool paused = false;
    int focused = PLAYER_APP__BUTTON_PLAY_PAUSE;
    bool volume_open = false;
    /* LEFT/RIGHT are dual-purpose (tap = move focus, hold = seek), which
     * input__wait() can't distinguish -- it only ever reports presses (see
     * its own doc comment: "release and change events are ignored"). Using
     * input__read() instead and tracking the held code/press time here
     * lets a still-held LEFT/RIGHT fire its seek the moment it crosses
     * PLAYER_APP__LONG_PRESS_MS, without waiting for a release that may not
     * come for seconds. */
    int32_t held_code = 0;
    uint64_t held_since_ms = 0;
    bool long_press_fired = false;
    for (;;) {
        if (!audio__file_is_open()) {
            /* The track finished on its own (not via BACK below) -- advance
             * to the next one if the folder has more, otherwise we're done. */
            if (playlist.count > 1) {
                playlist.current = (playlist.current + 1) % playlist.count;
                if (player__play_track(&playlist, title, sizeof(title)) != BRUCE_OK) break;
                paused = false;
                continue;
            }
            break;
        }

        (void)player__draw(title, &playlist, paused, focused, volume_open);

        bruce_input_event_t event;
        bruce_result_t input_result = input__read(&event, 80);

        if (held_code != 0 && !long_press_fired &&
            runtime__now() - held_since_ms >= PLAYER_APP__LONG_PRESS_MS) {
            if (held_code == BRUCE_INPUT_CODE_LEFT) player__seek_backward();
            else player__seek_forward();
            long_press_fired = true;
        }

        if (input_result == BRUCE_ERR_NOT_FOREGROUND) {
            held_code = 0;
            long_press_fired = false;
            if (runtime__delay(100) != BRUCE_OK) break;
            continue;
        }
        if (input_result == BRUCE_ERR_TIMEOUT) continue; /* just redraw/animate again */
        if (input_result != BRUCE_OK) break; /* e.g. input deinitialized during shutdown */

        if (event.action == BRUCE_INPUT_RELEASE) {
            if (event.code == held_code) {
                /* Released before the long-press threshold -- a tap. */
                if (!long_press_fired) player__move_focus(&focused, held_code == BRUCE_INPUT_CODE_LEFT ? -1 : 1);
                held_code = 0;
                long_press_fired = false;
            }
            continue;
        }
        if (event.action != BRUCE_INPUT_PRESS) continue; /* ignore CHANGE */

        if (volume_open) {
            switch (event.code) {
                case BRUCE_INPUT_CODE_PREV:
                case BRUCE_INPUT_CODE_LEFT:
                case BRUCE_INPUT_CODE_DOWN: player__change_volume(-PLAYER_APP__VOLUME_STEP); break;
                case BRUCE_INPUT_CODE_NEXT:
                case BRUCE_INPUT_CODE_RIGHT:
                case BRUCE_INPUT_CODE_UP: player__change_volume(PLAYER_APP__VOLUME_STEP); break;
                case BRUCE_INPUT_CODE_SELECT:
                case BRUCE_INPUT_CODE_BACK: volume_open = false; break;
                default: break;
            }
            continue;
        }

        switch (event.code) {
            case BRUCE_INPUT_CODE_BACK:
                goto done;
            case BRUCE_INPUT_CODE_SELECT:
                if (player__activate_focused(
                        focused, &playlist, title, sizeof(title), &paused, &volume_open
                    )) {
                    goto done;
                }
                break;
            case BRUCE_INPUT_CODE_PREV:
                player__move_focus(&focused, -1);
                break;
            case BRUCE_INPUT_CODE_NEXT:
                player__move_focus(&focused, 1);
                break;
            case BRUCE_INPUT_CODE_UP: player__change_volume(PLAYER_APP__VOLUME_STEP); break;
            case BRUCE_INPUT_CODE_DOWN: player__change_volume(-PLAYER_APP__VOLUME_STEP); break;
            case BRUCE_INPUT_CODE_LEFT:
            case BRUCE_INPUT_CODE_RIGHT:
                held_code = event.code;
                held_since_ms = event.timestamp_ms;
                long_press_fired = false;
                break;
            default:
                break;
        }
    }
done:
    (void)audio__file_stop();
    player__free_playlist(&playlist);
    return BRUCE_OK;
}
