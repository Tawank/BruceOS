include("${CMAKE_CURRENT_LIST_DIR}/doomgeneric_sources.cmake")

idf_component_register(
    SRCS "main.c" "doom_video.c" "doom_input.c" "doom_sound.c" ${DOOMGENERIC_SOURCES}
    INCLUDE_DIRS "." "compat" "${DOOMGENERIC_DIR}" "../../../include" "../../../../src")

# Must reach every source: doomgeneric.c sizes DG_ScreenBuffer and i_video.c picks its scale from these.
target_compile_definitions(${COMPONENT_LIB} PRIVATE DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)

# doomgeneric is a 1990s-2020s-vintage C codebase (chocolate-doom-derived)
# built for permissive desktop toolchains; ESP-IDF's default warning set
# promotes several of its long-standing, harmless patterns to hard errors.
# None of these were introduced by fetching it here -- they're present in
# upstream doomgeneric against any sufficiently strict compiler.
#
# FEATURE_SOUND routes i_sound.c through DG_sound_module/DG_music_module
# (doom_sound.c) instead of compiling the sound layer out.
target_compile_definitions(${COMPONENT_LIB} PRIVATE FEATURE_SOUND)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=implicit-function-declaration
    -Wno-error=implicit-int
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=stringop-overflow
    -Wno-error=sign-compare
    -Wno-error=type-limits
    -Wno-error=unused-but-set-variable
    -Wno-error=unused-variable
    -Wno-error=unused-function
    -Wno-error=unused-value
    -Wno-error=maybe-uninitialized
    -Wno-error=array-bounds
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-conversion
    -Wno-error=enum-conversion
    -Wno-error=missing-field-initializers
    -Wno-error=unterminated-string-initialization
    -Wno-error=unused-const-variable
    -Wno-error=implicit-fallthrough
    -Wno-error=unused-but-set-parameter
    -Wno-error=absolute-value
    -Wno-error=dangling-pointer
    -O2)
