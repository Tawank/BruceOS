#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core_sdk/result.h"

/**
 * @brief Identifies what a file is: description, MIME type, icon, and which
 * app_runner program opens it.
 *
 * Shared by app_runner (choosing a program to run a path with), filemanager
 * (icons and "Open"), and the `file` command. A path is identified in this
 * order, each tier only consulted if the previous one found nothing:
 *
 *   1. Directory.
 *   2. Extension, against the user-editable "/config/extensions.conf"
 *      table (falls back to a built-in default table - see
 *      embedded_resources/json/extensions.json - the first time the file
 *      is missing or invalid).
 *   3. Magic bytes sampled from the start of the file (a small built-in
 *      signature table: PNG, JPEG, GIF, BMP, ZIP, GZIP, ELF, WASM, RIFF/WAV,
 *      FLAC, OggS, 7z, RAR).
 *   4. A `#!` shebang line, matching the interpreter name against the same
 *      extensions.conf table's "interpreters" lists.
 *   5. A text/binary heuristic over the sampled bytes (any NUL or unusual
 *      control byte counts as binary).
 *
 * Tier 2 is authoritative for `program`/`icon`/`mimetype` when it matches -
 * it is curated and user-editable, so it wins even when a magic-byte tier
 * below it could also have matched (e.g. a ".conf" file is plain-text
 * bytes, but should keep its own icon rather than falling back to "ASCII
 * text").
 */

#define BRUCE_FILETYPE_DESCRIPTION_MAX 64
#define BRUCE_FILETYPE_MIME_MAX 64
#define BRUCE_FILETYPE_ICON_MAX 32
#define BRUCE_FILETYPE_PROGRAM_MAX 32

typedef struct {
    /* Short human-readable description, e.g. "PNG image", "POSIX shell
     * script", "ASCII text", "data". Never empty. */
    char description[BRUCE_FILETYPE_DESCRIPTION_MAX];
    /* MIME type, e.g. "image/png"; "" if none is known for this file. */
    char mimetype[BRUCE_FILETYPE_MIME_MAX];
    /* icon__get()-compatible icon name; "file" if nothing more specific matched. */
    char icon[BRUCE_FILETYPE_ICON_MAX];
    /* app_runner program name to open this file with; "" if none configured. */
    char program[BRUCE_FILETYPE_PROGRAM_MAX];
    bool is_directory;
    /* Best-effort text/binary guess, only meaningful when is_directory is false. */
    bool is_binary;
} bruce_filetype_info_t;

/**
 * @brief Looks up description/mimetype/icon/program from the extension
 * table alone - no storage permission required, no I/O, and no fallback to
 * magic-byte/shebang/text-heuristic detection.
 *
 * This is tier 2 of filetype__identify() in isolation: the same answer a
 * matching extension would give there, just without ever opening the file,
 * for callers where that matters - app_runner's launch fast path (the
 * common case of a recognized extension shouldn't cost a read), and
 * per-row icon lookups when listing a whole directory (filemanager,
 * dialog's file picker), where opening every entry just for its icon
 * isn't affordable. is_directory and is_binary are always false; every
 * string field is "" (icon is "file") when the path's extension has no
 * configured entry.
 *
 * @param path Path whose extension is looked up.
 * @param out_info Receives the lookup result.
 */
bruce_result_t filetype__lookup_extension(const char *path, bruce_filetype_info_t *out_info);

/**
 * @brief Returns the configured icon for a path's extension.
 *
 * Same lookup as filetype__lookup_extension()'s icon field, "file" as a
 * default, but returned as a pointer into Core's own cached config rather
 * than copied into an out-struct: callers building a whole list of icons
 * at once (a directory listing's per-row icons, kept live together until
 * the list is rendered) can hold every returned pointer simultaneously,
 * which a shared scratch buffer couldn't support. The returned string is
 * owned by Core and valid for the lifetime of the firmware.
 *
 * @param path File path whose extension selects the icon, or NULL.
 */
const char *filetype__icon_for_path(const char *path);

/**
 * @brief Identifies a file already on storage.
 *
 * Reads a small header sample (and directory-ness) via the calling
 * process's normal storage permission - see storage__open()'s permission
 * notes. Prefer filetype__identify_bytes() when the caller already has the
 * file's bytes in memory, to avoid a second read.
 *
 * @param path Absolute or "./"-relative path to identify.
 * @param out_info Receives the identification.
 * @permission storage
 */
bruce_result_t filetype__identify(const char *path, bruce_filetype_info_t *out_info);

/**
 * @brief Identifies a file from bytes already in memory.
 *
 * `bytes`/`size` should be a prefix of the file - a few hundred bytes is
 * enough for every detection tier here (magic signatures are at most 12
 * bytes in, shebang parsing only needs the first line). `path` still
 * drives the extension tier and is otherwise unused; pass NULL (or "") when
 * identifying a floating buffer with no associated path, e.g. a download
 * or clipboard buffer - detection then skips straight to the magic-byte/
 * shebang/text-heuristic tiers. This never touches storage itself.
 *
 * @param path Path associated with bytes, or NULL/"" if there isn't one.
 * @param bytes Prefix of the file's contents.
 * @param size Number of bytes in bytes.
 * @param out_info Receives the identification.
 */
bruce_result_t filetype__identify_bytes(
    const char *path, const uint8_t *bytes, size_t size, bruce_filetype_info_t *out_info
);
