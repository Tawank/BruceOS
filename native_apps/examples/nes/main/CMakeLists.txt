# The dependency manager installs the pinned fork inside this NES project.
# project_elf() reliably retains only libmain.a, so compile the fetched core
# sources into main rather than linking Nofrendo as a second archive.
include("${CMAKE_CURRENT_LIST_DIR}/../managed_components/nofrendo/nofrendo_sources.cmake")

idf_component_register(
    SRCS "main.c" "nes_osd.c" "nes_video.c" "nes_input.c" "nes_sound.c" "bruce_stdio.c" ${NOFRENDO_SOURCES}
    INCLUDE_DIRS "." "${NOFRENDO_DIR}" "../../../include" "../../../../src"
    REQUIRES nofrendo)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=attributes
    -Wno-error=format
    -Wno-error=implicit-fallthrough
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-to-pointer-cast
    -Wno-error=nonnull-compare
    -Wno-error=pointer-to-int-cast
    -Wno-error=stringop-truncation
    -Wno-error=unused-const-variable
    -Wno-unused-function
    # Keep the Bruce OSD glue optimized consistently with the separately
    # compiled Nofrendo component, whose upstream CMakeLists also selects -O2.
    # Emulator frame time is CPU-bound on ESP32-S3.
    -O2)
