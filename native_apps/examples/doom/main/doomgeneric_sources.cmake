# Upstream doomgeneric isn't an ESP-IDF component (no CMakeLists.txt of its
# own), and ESP-IDF's build system hard-errors ("does not contain a
# component") if a managed_components/* entry isn't one -- unlike a plain,
# not-explicitly-required component-directory miss, which just gets a
# skip notice. So this fetches it directly at configure time instead of
# through idf_component.yml, into the build directory (so `idf.py
# fullclean` cleans it up naturally, and re-running configure is a cheap
# no-op once the pinned commit is already checked out).
set(DOOMGENERIC_UPSTREAM_DIR "${CMAKE_BINARY_DIR}/doomgeneric_upstream")
set(DOOMGENERIC_UPSTREAM_COMMIT "dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284")

if(NOT EXISTS "${DOOMGENERIC_UPSTREAM_DIR}/.git")
    execute_process(
        COMMAND git clone --quiet https://github.com/ozkl/doomgeneric.git "${DOOMGENERIC_UPSTREAM_DIR}"
        RESULT_VARIABLE DOOMGENERIC_CLONE_RESULT)
    if(NOT DOOMGENERIC_CLONE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to clone https://github.com/ozkl/doomgeneric.git")
    endif()
endif()
execute_process(
    COMMAND git -C "${DOOMGENERIC_UPSTREAM_DIR}" checkout --quiet "${DOOMGENERIC_UPSTREAM_COMMIT}"
    RESULT_VARIABLE DOOMGENERIC_CHECKOUT_RESULT)
if(NOT DOOMGENERIC_CHECKOUT_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to check out pinned doomgeneric commit ${DOOMGENERIC_UPSTREAM_COMMIT}")
endif()

set(DOOMGENERIC_DIR "${DOOMGENERIC_UPSTREAM_DIR}/doomgeneric")

# Upstream's M_FileExists() (m_misc.c) checks `errno == EISDIR` after a
# failed fopen(). On this toolchain `errno` expands to `_tls_errno`, a real
# C11 _Thread_local variable -- an R_XTENSA_TLS_TPOFF relocation, a kind the
# Bruce ELF loader doesn't support at all (no such constant even appears in
# components/elf_loader/src/arch/esp_elf_xtensa.c's relocation-type enum).
# Patched in place at configure time (idempotent: skipped once already
# applied, so re-running configure against an already-fetched checkout is
# a no-op) rather than forking upstream just for this one line -- asks the
# same "does this path exist at all" question via access(), which needs no
# TLS relocation and is a strict superset of the original intent (existing-
# but-unreadable-for-another-reason now also counts, never a regression for
# this call site's only use, WAD-path validation in d_iwad.c).
set(DOOMGENERIC_M_MISC "${DOOMGENERIC_DIR}/m_misc.c")
if(EXISTS "${DOOMGENERIC_M_MISC}")
    file(READ "${DOOMGENERIC_M_MISC}" DOOMGENERIC_M_MISC_CONTENTS)
    if(NOT DOOMGENERIC_M_MISC_CONTENTS MATCHES "access\\(filename, F_OK\\)")
        string(REPLACE
            "#include <errno.h>"
            "#include <errno.h>\n#include <unistd.h>"
            DOOMGENERIC_M_MISC_CONTENTS "${DOOMGENERIC_M_MISC_CONTENTS}")
        string(REPLACE
            "return errno == EISDIR;"
            "return access(filename, F_OK) == 0;"
            DOOMGENERIC_M_MISC_CONTENTS "${DOOMGENERIC_M_MISC_CONTENTS}")
        file(WRITE "${DOOMGENERIC_M_MISC}" "${DOOMGENERIC_M_MISC_CONTENTS}")
    endif()
endif()

# Upstream ships several desktop platform frontends and their sound/CD-audio
# backends alongside the portable engine; this port supplies its own
# frontend (main.c + doom_video.c + doom_input.c, following the same
# DG_Init/DG_DrawFrame/DG_SleepMs/DG_GetTicksMs/DG_GetKey interface they
# implement) instead, and disables audio entirely (see ../README.md), so
# none of these compile or link here -- excluding them in this file (kept
# in this app's own tree, not upstream's) is what lets the engine be
# consumed straight from ozkl/doomgeneric with no fork to maintain.
file(GLOB DOOMGENERIC_SOURCES "${DOOMGENERIC_DIR}/*.c")
list(REMOVE_ITEM DOOMGENERIC_SOURCES
    "${DOOMGENERIC_DIR}/doomgeneric_allegro.c"
    "${DOOMGENERIC_DIR}/doomgeneric_emscripten.c"
    "${DOOMGENERIC_DIR}/doomgeneric_linuxvt.c"
    "${DOOMGENERIC_DIR}/doomgeneric_sdl.c"
    "${DOOMGENERIC_DIR}/doomgeneric_soso.c"
    "${DOOMGENERIC_DIR}/doomgeneric_sosox.c"
    "${DOOMGENERIC_DIR}/doomgeneric_win.c"
    "${DOOMGENERIC_DIR}/doomgeneric_xlib.c"
    "${DOOMGENERIC_DIR}/i_allegromusic.c"
    "${DOOMGENERIC_DIR}/i_allegrosound.c"
    "${DOOMGENERIC_DIR}/i_cdmus.c"
    "${DOOMGENERIC_DIR}/i_sdlmusic.c"
    "${DOOMGENERIC_DIR}/i_sdlsound.c"
    "${DOOMGENERIC_DIR}/icon.c")
