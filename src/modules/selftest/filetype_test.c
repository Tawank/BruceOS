#include "filetype_test.h"

#include "core_sdk/filetype.h"
#include "core_sdk/storage.h"

#include <stdio.h>
#include <string.h>

bool selftest__run_filetype_extension_case(void) {
    bruce_filetype_info_t sh_info;
    bruce_result_t sh_result = filetype__lookup_extension("/bin/foo.sh", &sh_info);
    bool sh_ok = sh_result == BRUCE_OK && strcmp(sh_info.program, "shell") == 0 &&
                 strcmp(sh_info.icon, "file-code") == 0 && strcmp(sh_info.mimetype, "text/x-shellscript") == 0 &&
                 !sh_info.is_directory;

    /* An extension with no configured entry falls back to the "file" icon
     * and every other field empty, never NULL/garbage. */
    bruce_filetype_info_t unknown_info;
    bruce_result_t unknown_result = filetype__lookup_extension("/bin/foo.xyz", &unknown_info);
    bool unknown_ok = unknown_result == BRUCE_OK && strcmp(unknown_info.icon, "file") == 0 &&
                       unknown_info.program[0] == '\0' && unknown_info.mimetype[0] == '\0';

    bool ok = sh_ok && unknown_ok;
    printf(
        "[selftest] filetype/extension: %s (sh_program=%s unknown_icon=%s)\n", ok ? "OK" : "FAIL", sh_info.program,
        unknown_info.icon
    );
    return ok;
}

bool selftest__run_filetype_magic_bytes_case(void) {
    static const uint8_t png_bytes[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 0};
    bruce_filetype_info_t png_info;
    bruce_result_t png_result = filetype__identify_bytes(NULL, png_bytes, sizeof(png_bytes), &png_info);
    bool png_ok = png_result == BRUCE_OK && strcmp(png_info.description, "PNG image") == 0 &&
                  strcmp(png_info.mimetype, "image/png") == 0 && strcmp(png_info.icon, "file-image") == 0 &&
                  png_info.is_binary;

    static const uint8_t elf_bytes[] = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0};
    bruce_filetype_info_t elf_info;
    bruce_result_t elf_result = filetype__identify_bytes(NULL, elf_bytes, sizeof(elf_bytes), &elf_info);
    bool elf_ok = elf_result == BRUCE_OK && strcmp(elf_info.description, "ELF executable") == 0 &&
                  elf_info.is_binary;

    bool ok = png_ok && elf_ok;
    printf(
        "[selftest] filetype/magic_bytes: %s (png=%s elf=%s)\n", ok ? "OK" : "FAIL", png_info.description,
        elf_info.description
    );
    return ok;
}

bool selftest__run_filetype_shebang_case(void) {
    /* "#!/usr/bin/env bash" - the interpreter is env's argument, not "env"
     * itself, and matches the shell entry's "interpreters" list the same
     * way a direct "#!/bin/bash" would. */
    static const char env_script[] = "#!/usr/bin/env bash\necho hi\n";
    bruce_filetype_info_t env_info;
    bruce_result_t env_result =
        filetype__identify_bytes(NULL, (const uint8_t *)env_script, strlen(env_script), &env_info);
    bool env_ok = env_result == BRUCE_OK && strcmp(env_info.program, "shell") == 0 && !env_info.is_binary;

    static const char direct_script[] = "#!/bin/sh\necho hi\n";
    bruce_filetype_info_t direct_info;
    bruce_result_t direct_result =
        filetype__identify_bytes(NULL, (const uint8_t *)direct_script, strlen(direct_script), &direct_info);
    bool direct_ok = direct_result == BRUCE_OK && strcmp(direct_info.program, "shell") == 0;

    /* An interpreter nothing configures still gets a synthesized
     * description, just no program to launch it with. */
    static const char unknown_script[] = "#!/usr/bin/lua\nprint('hi')\n";
    bruce_filetype_info_t unknown_info;
    bruce_result_t unknown_result =
        filetype__identify_bytes(NULL, (const uint8_t *)unknown_script, strlen(unknown_script), &unknown_info);
    bool unknown_ok =
        unknown_result == BRUCE_OK && unknown_info.program[0] == '\0' && strstr(unknown_info.description, "lua") != NULL;

    bool ok = env_ok && direct_ok && unknown_ok;
    printf(
        "[selftest] filetype/shebang: %s (env_program=%s direct_program=%s unknown_desc=%s)\n", ok ? "OK" : "FAIL",
        env_info.program, direct_info.program, unknown_info.description
    );
    return ok;
}

bool selftest__run_filetype_text_binary_case(void) {
    static const char text_bytes[] = "hello world\n";
    bruce_filetype_info_t text_info;
    bruce_result_t text_result =
        filetype__identify_bytes(NULL, (const uint8_t *)text_bytes, strlen(text_bytes), &text_info);
    bool text_ok = text_result == BRUCE_OK && !text_info.is_binary &&
                   strcmp(text_info.description, "ASCII text") == 0;

    static const uint8_t binary_bytes[] = {'a', 'b', 0x00, 'c', 'd'};
    bruce_filetype_info_t binary_info;
    bruce_result_t binary_result = filetype__identify_bytes(NULL, binary_bytes, sizeof(binary_bytes), &binary_info);
    bool binary_ok =
        binary_result == BRUCE_OK && binary_info.is_binary && strcmp(binary_info.description, "data") == 0;

    bool ok = text_ok && binary_ok;
    printf("[selftest] filetype/text_binary: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool selftest__run_filetype_identify_path_case(void) {
    /* No image extension, so this can only be identified by its magic
     * bytes -- proves filetype__identify() actually reads the file rather
     * than only ever consulting the extension table. */
    static const char path[] = "/selftest_filetype_tmp.bin";
    static const uint8_t png_bytes[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

    bruce_file_id_t file = BRUCE_FILE_ID_INVALID;
    bruce_result_t open_result =
        storage__open(path, BRUCE_STORAGE_OPEN_WRITE | BRUCE_STORAGE_OPEN_CREATE | BRUCE_STORAGE_OPEN_TRUNCATE, &file);
    size_t written = 0;
    bruce_result_t write_result =
        open_result == BRUCE_OK ? storage__write(file, png_bytes, sizeof(png_bytes), &written) : open_result;
    if (open_result == BRUCE_OK) storage__close(file);

    bruce_filetype_info_t file_info;
    bruce_result_t identify_result = filetype__identify(path, &file_info);
    bool file_ok = identify_result == BRUCE_OK && !file_info.is_directory &&
                    strcmp(file_info.description, "PNG image") == 0;
    (void)storage__remove(path);

    bruce_filetype_info_t root_info;
    bruce_result_t root_result = filetype__identify("/", &root_info);
    bool root_ok = root_result == BRUCE_OK && root_info.is_directory;

    bool ok = open_result == BRUCE_OK && write_result == BRUCE_OK && file_ok && root_ok;
    printf(
        "[selftest] filetype/identify_path: %s (file_desc=%s root_is_dir=%d)\n", ok ? "OK" : "FAIL",
        file_info.description, root_info.is_directory
    );
    return ok;
}
