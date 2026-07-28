#include "bnu_app.h"

#include <stdio.h>
#include <string.h>

#include "core_sdk/memory.h"
#include "core_sdk/result.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

static char s_working_directory[BRUCE_STORAGE_PATH_MAX] = "/";

const char *bnu__get_working_directory(void) { return s_working_directory; }

static int bnu__usage(const char *command, const char *arguments) {
    stdio__printf("usage: %s%s%s\n", command, arguments[0] != '\0' ? " " : "", arguments);
    return BRUCE_ERR_INVALID_ARGUMENT;
}

static bool bnu__resolve_path(const char *path, char *out_path) {
    char combined[BRUCE_STORAGE_PATH_MAX * 2];
    if (path == NULL || path[0] == '\0') path = s_working_directory;
    int written = path[0] == '/' ? snprintf(combined, sizeof(combined), "%s", path)
                                 : snprintf(
                                       combined,
                                       sizeof(combined),
                                       "%s%s%s",
                                       s_working_directory,
                                       strcmp(s_working_directory, "/") == 0 ? "" : "/",
                                       path
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

int bnu_pwd_app_main(int argc, char **argv) {
    (void)argv;
    if (argc != 0) return bnu__usage("pwd", "");
    stdio__printf("%s\n", s_working_directory);
    return BRUCE_OK;
}

int bnu_cd_app_main(int argc, char **argv) {
    if (argc > 1) return bnu__usage("cd", "[directory]");
    char path[BRUCE_STORAGE_PATH_MAX];
    if (!bnu__resolve_path(argc == 1 ? argv[0] : "/", path)) return BRUCE_ERR_INVALID_PATH;
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) {
        stdio__printf("cd: %s: error %d\n", path, result);
        return result;
    }
    snprintf(s_working_directory, sizeof(s_working_directory), "%s", path);
    return BRUCE_OK;
}

int bnu_ls_app_main(int argc, char **argv) {
    if (argc > 1) return bnu__usage("ls", "[path]");
    char path[BRUCE_STORAGE_PATH_MAX];
    if (!bnu__resolve_path(argc == 1 ? argv[0] : NULL, path)) return BRUCE_ERR_INVALID_PATH;
    size_t count = 0;
    bruce_result_t result = storage__list(path, NULL, 0, &count);
    if (result != BRUCE_OK) {
        stdio__printf("ls: %s: error %d\n", path, result);
        return result;
    }
    if (count == 0) return BRUCE_OK;
    bruce_storage_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = storage__list(path, entries, count, &count);
    if (result == BRUCE_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].type == BRUCE_STORAGE_ENTRY_DIRECTORY) {
                stdio__printf("%-10s %s/\n", "<dir>", entries[i].name);
            } else {
                stdio__printf("%10u %s\n", (unsigned)entries[i].size, entries[i].name);
            }
        }
    }
    memory__free(entries);
    return result;
}

static void bnu__print_memory_row(const char *name, size_t total, size_t free_size, size_t largest) {
    stdio__printf(
        "%-5s %7u %7u %6u %6u\n",
        name,
        (unsigned)total,
        (unsigned)(total - free_size),
        (unsigned)free_size,
        (unsigned)largest
    );
}

int bnu_free_app_main(int argc, char **argv) {
    (void)argv;
    if (argc != 0) return bnu__usage("free", "");
    bruce_memory_stats_t stats;
    bruce_result_t result = memory__get_stats(&stats);
    if (result != BRUCE_OK) return result;
    stdio__printf("%-5s %7s %7s %6s %6s\n", "mem", "total", "used", "free", "lrgst");
    bnu__print_memory_row("int", stats.internal_total, stats.internal_free, stats.internal_largest_block);
    if (stats.psram_total > 0) {
        bnu__print_memory_row("psram", stats.psram_total, stats.psram_free, stats.psram_largest_block);
    }
    return BRUCE_OK;
}

int bnu_mkdir_app_main(int argc, char **argv) {
    if (argc != 1) return bnu__usage("mkdir", "<directory>");
    char path[BRUCE_STORAGE_PATH_MAX];
    if (!bnu__resolve_path(argv[0], path)) return BRUCE_ERR_INVALID_PATH;
    bruce_result_t result = storage__mkdir(path);
    if (result != BRUCE_OK) {
        stdio__printf("mkdir: %s: error %d\n", path, result);
        return result;
    }
    return BRUCE_OK;
}

int bnu_touch_app_main(int argc, char **argv) {
    if (argc != 1) return bnu__usage("touch", "<file>");
    char path[BRUCE_STORAGE_PATH_MAX];
    if (!bnu__resolve_path(argv[0], path)) return BRUCE_ERR_INVALID_PATH;
    bruce_file_id_t file;
    bruce_result_t result =
        storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE, &file);
    if (result != BRUCE_OK) {
        stdio__printf("touch: %s: error %d\n", path, result);
        return result;
    }
    storage__close(file);
    return BRUCE_OK;
}
