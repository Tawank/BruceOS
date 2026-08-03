#include "bnu_app.h"

#include <stdio.h>
#include <string.h>

#include "args.h"
#include "core_sdk/disk.h"
#include "core_sdk/memory.h"
#include "core_sdk/process.h"
#include "core_sdk/result.h"
#include "core_sdk/runtime.h"
#include "core_sdk/stdio.h"
#include "core_sdk/storage.h"

static char s_working_directory[BRUCE_STORAGE_PATH_MAX] = "/";

const char *bnu__get_working_directory(void) { return s_working_directory; }

static int bnu__parse_failure(ArgParser *parser) {
    ap_status_t status = ap_get_status(parser);
    ap_free(parser);
    if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
    return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
}

static ArgParser *bnu__new_parser(const char *helptext) {
    ArgParser *parser = ap_new_parser();
    if (parser != NULL) ap_set_helptext(parser, helptext);
    return parser;
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
    ArgParser *parser = bnu__new_parser("Print the current working directory.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);
    stdio__printf("%s\n", s_working_directory);
    return BRUCE_OK;
}

int bnu_cd_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Change the current working directory.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "directory", "Directory path (defaults to /)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    char *directory = ap_get_arg(parser, "directory");
    bool resolved = bnu__resolve_path(directory != NULL ? directory : "/", path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
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
    ArgParser *parser = bnu__new_parser("List files and directories.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "path", "Path to list (defaults to the working directory)");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "path"), path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
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

static void bnu__format_block_size(uint64_t bytes, char *output, size_t capacity) {
    static const char units[] = {'B', 'K', 'M', 'G', 'T'};
    uint64_t divisor = 1;
    size_t unit = 0;
    while (unit + 1 < sizeof(units) && bytes >= divisor * 1024) {
        divisor *= 1024;
        unit++;
    }
    uint64_t whole = bytes / divisor;
    uint64_t tenth = ((bytes % divisor) * 10) / divisor;
    if (unit == 0 || tenth == 0) {
        snprintf(output, capacity, "%llu%c", (unsigned long long)whole, units[unit]);
    } else {
        snprintf(
            output, capacity, "%llu.%llu%c", (unsigned long long)whole, (unsigned long long)tenth, units[unit]
        );
    }
}

int bnu_lsblk_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List block devices and partitions.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);

    size_t count = 0;
    bruce_result_t result = disk__list(NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_disk_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = disk__list(entries, count, &count);
    if (result != BRUCE_OK) {
        memory__free(entries);
        return result;
    }

    stdio__printf("%-18s %2s %6s %2s %-4s %s\n", "NAME", "RM", "SIZE", "RO", "TYPE", "MOUNTPOINTS");
    for (size_t i = 0; i < count; ++i) {
        char size[16];
        char name[BRUCE_DISK_NAME_MAX + 3];
        bnu__format_block_size(entries[i].size, size, sizeof(size));
        snprintf(
            name,
            sizeof(name),
            "%s%s",
            entries[i].type == BRUCE_DISK_TYPE_PARTITION
                ? (i + 1 == count || entries[i + 1].parent[0] == '\0' ? "`-" : "|-")
                : "",
            entries[i].name
        );
        stdio__printf(
            "%-18s %2u %6s %2u %-4s %s\n",
            name,
            entries[i].removable ? 1u : 0u,
            size,
            entries[i].read_only ? 1u : 0u,
            entries[i].type == BRUCE_DISK_TYPE_PARTITION ? "part" : "disk",
            entries[i].mount_point
        );
    }
    memory__free(entries);
    return BRUCE_OK;
}

static int bnu__list_mounts(void) {
    size_t count = 0;
    bruce_result_t result = disk__list(NULL, 0, &count);
    if (result != BRUCE_OK) return result;
    bruce_disk_entry_t *entries = memory__malloc(count * sizeof(*entries));
    if (entries == NULL) return BRUCE_ERR_NO_MEMORY;
    result = disk__list(entries, count, &count);
    if (result == BRUCE_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].mount_point[0] == '\0') continue;
            stdio__printf(
                "%s on %s type %s (rw)\n",
                entries[i].name,
                entries[i].mount_point,
                entries[i].type == BRUCE_DISK_TYPE_PARTITION ? "littlefs" : "fat"
            );
        }
    }
    memory__free(entries);
    return result;
}

int bnu_mount_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("List mounted filesystems or mount a block device.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_optional_arg(parser, "device", "Block device name (currently sd0)");
    ap_add_optional_arg(parser, "mount-point", "Mount point (defaults to /sdcard)");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *device = ap_get_arg(parser, "device");
    const char *mount_point = ap_get_arg(parser, "mount-point");
    if (device == NULL) {
        ap_free(parser);
        return bnu__list_mounts();
    }
    bruce_result_t result = disk__mount(device, mount_point != NULL ? mount_point : "/sdcard");
    if (result != BRUCE_OK) stdio__printf("mount: %s: error %d\n", device, result);
    ap_free(parser);
    return result;
}

int bnu_unmount_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Unmount a block device or mount point.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "target", "Block device name or mount point");
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    const char *target = ap_get_arg(parser, "target");
    bruce_result_t result = disk__unmount(target);
    if (result != BRUCE_OK) stdio__printf("unmount: %s: error %d\n", target, result);
    ap_free(parser);
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
    ArgParser *parser = bnu__new_parser("Show internal memory and PSRAM usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);
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

static const char *bnu__process_state_name(bruce_process_state_t state) {
    switch (state) {
        case BRUCE_PROCESS_STARTING: return "start";
        case BRUCE_PROCESS_FOREGROUND: return "fore";
        case BRUCE_PROCESS_BACKGROUND: return "back";
        case BRUCE_PROCESS_PAUSED: return "pause";
        case BRUCE_PROCESS_STOPPING: return "stop";
        default: return "?";
    }
}

int bnu_top_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Show runtime process resource usage.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    ap_free(parser);

    bruce_process_snapshot_t processes[16];
    size_t process_count = 0;
    bruce_result_t result =
        process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
    if (result != BRUCE_OK) return result;
    result = runtime__delay(250);
    if (result != BRUCE_OK) return result;
    result = process__list(processes, sizeof(processes) / sizeof(processes[0]), &process_count);
    if (result != BRUCE_OK) return result;

    stdio__printf("\n%1s %2s %3s %4s %4s %4s %s\n", "s", "id", "cpu", "stck", "heap", "swap", "name");
    for (size_t i = 0; i < process_count; ++i) {
        uint32_t stack_used_bytes = processes[i].stack_total_bytes > processes[i].stack_high_water_bytes
                                        ? processes[i].stack_total_bytes - processes[i].stack_high_water_bytes
                                        : 0;
        stdio__printf(
            "%1s %2u %3u %4u %4u %4u %s\n",
            bnu__process_state_name(processes[i].state),
            (unsigned)processes[i].id,
            (unsigned)processes[i].cpu_percent,
            (unsigned)stack_used_bytes,
            (unsigned)processes[i].memory_bytes,
            (unsigned)processes[i].swap_bytes,
            processes[i].name
        );
    }
    return BRUCE_OK;
}

int bnu_mkdir_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Create a directory.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "directory", "Directory path to create");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "directory"), path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
    bruce_result_t result = storage__mkdir(path);
    if (result != BRUCE_OK) {
        stdio__printf("mkdir: %s: error %d\n", path, result);
        return result;
    }
    return BRUCE_OK;
}

int bnu_touch_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Create a file if it does not exist.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "file", "File path to create");
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);
    char path[BRUCE_STORAGE_PATH_MAX];
    bool resolved = bnu__resolve_path(ap_get_arg(parser, "file"), path);
    ap_free(parser);
    if (!resolved) return BRUCE_ERR_INVALID_PATH;
    bruce_file_id_t file;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE, &file);
    if (result != BRUCE_OK) {
        stdio__printf("touch: %s: error %d\n", path, result);
        return result;
    }
    storage__close(file);
    return BRUCE_OK;
}

static bruce_result_t bnu__cat_file(const char *path) {
    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t result = storage__open(path, BRUCE_STORAGE_OPEN_READ, &file);
    if (result != BRUCE_OK) return result;

    unsigned char buffer[256];
    while (result == BRUCE_OK) {
        size_t read_size = 0;
        result = storage__read(file, buffer, sizeof(buffer), &read_size);
        if (result != BRUCE_OK || read_size == 0) break;
        result = stdio__write(buffer, read_size);
    }

    bruce_result_t close_result = storage__close(file);
    return result != BRUCE_OK ? result : close_result;
}

int bnu_cat_app_main(int argc, char **argv) {
    ArgParser *parser = bnu__new_parser("Print file contents.");
    if (parser == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_add_required_arg(parser, "file", "File path to print");
    ap_allow_extra_args(parser);
    ap_unknown_options_as_args(parser);
    if (argc < 1 || !ap_parse(parser, argc, argv)) return bnu__parse_failure(parser);

    int path_count = ap_count_args(parser);
    for (int i = 0; i < path_count; ++i) {
        char path[BRUCE_STORAGE_PATH_MAX];
        if (!bnu__resolve_path(ap_get_arg_at_index(parser, i), path)) {
            ap_free(parser);
            return BRUCE_ERR_INVALID_PATH;
        }
        bruce_result_t result = bnu__cat_file(path);
        if (result != BRUCE_OK) {
            stdio__printf("cat: %s: error %d\n", path, result);
            ap_free(parser);
            return result;
        }
    }

    ap_free(parser);
    return BRUCE_OK;
}
