#include "ffplay_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core_sdk/audio.h"
#include "core_sdk/environment.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

/* Stand-in for ffplay, not a port: there's no mp3/compressed-audio decoder
 * anywhere in this codebase. A local .wav plays for real via the core
 * audio__file_play() API (core/audio/audio.c -- decoding/resampling/stream
 * plumbing lives there now, not here, so any other app can reuse it the
 * same way). Anything else this command is asked to open (a URL, a
 * non-wav file, an unsupported WAV encoding) is a real error, not silently
 * accepted. ffmpeg's flags are single-dash and multi-character (e.g.
 * "-hide_banner"), which doesn't fit ArgParser's getopt-style short-option
 * bundling, so this scans argv by hand instead: every "-xxx" token is
 * accepted and ignored except "-loglevel" (which also consumes its value)
 * and "-h"/"--help", and the first non-flag token is taken as the target. */

static void ffplay__print_help(void) {
    stdio__printf(
        "Usage: ffplay [options] <file>\n"
        "\n"
        "Minimal ffplay stand-in -- not a port. Only a local .wav file\n"
        "(8- or 16-bit PCM, mono or stereo, any sample rate) is supported;\n"
        "it plays for real through the board's audio output. Anything\n"
        "else (a URL, a non-wav file, an unsupported WAV encoding) is an\n"
        "error.\n"
        "\n"
        "  -h, --help       Show this help and exit\n"
        "  -hide_banner     Accepted, ignored (ffmpeg compatibility)\n"
        "  -loglevel LEVEL  Accepted, ignored\n"
        "  -autoexit        Accepted, ignored\n"
        "  -nodisp          Accepted, ignored\n"
        "  -nostats         Accepted, ignored\n"
    );
}

/* Resolves `path` against $PWD the same way the shell's own builtins do,
 * since ffplay runs as a plain external command with no path-resolution
 * help from the caller. Duplicated in miniature rather than shared with
 * bnu's own bnu__resolve_path(): that helper is bnu-internal and this
 * module intentionally doesn't depend on bnu. */
static bool ffplay__resolve_path(const char *path, char *out_path) {
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

int ffplay_app_main(int argc, char **argv) {
    const char *target = NULL;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            ffplay__print_help();
            return BRUCE_OK;
        }
        if (strcmp(arg, "-loglevel") == 0) {
            i++;
            continue;
        }
        if (arg[0] == '-') continue;
        if (target == NULL) target = arg;
    }
    if (target == NULL) {
        stdio__printf("ffplay: no input file\n");
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    char path[BRUCE_STORAGE_PATH_MAX];
    if (!ffplay__resolve_path(target, path)) {
        stdio__printf("ffplay: %s: path too long\n", target);
        return BRUCE_ERR_INVALID_ARGUMENT;
    }

    bruce_result_t played = audio__file_play(path);
    if (played != BRUCE_OK) {
        stdio__printf(
            "ffplay: %s: %s\n", target,
            played == BRUCE_ERR_UNSUPPORTED ? "unsupported file (only 8- or 16-bit PCM .wav is supported)"
                                             : "could not be played"
        );
        return 1;
    }

    /* audio__file_play() itself only starts playback in the background and
     * returns right away; wait here so ffplay still behaves like a normal
     * foreground command that blocks until the track ends (matching real
     * ffplay without -nodisp -- a script can still background this whole
     * process with "&" the same way it would real ffplay). If ffplay is
     * killed first, stop the playback it started rather than leaving it
     * running with nothing left to stop it. */
    while (audio__file_is_playing() || audio__file_is_open()) {
        if (process__current_signal() != 0) {
            (void)audio__file_stop();
            break;
        }
        if (runtime__sleep(100u) != BRUCE_OK && process__current_signal() == 0) break;
    }
    return 0;
}
