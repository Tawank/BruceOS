include("${CMAKE_CURRENT_LIST_DIR}/../../../../components/nofrendo/nofrendo_sources.cmake")

# project_elf() links only libmain.a, so compile the reusable component sources
# into this archive instead of relying on an ordinary component dependency.
idf_component_register(
    SRCS "main.c" "nes_osd.c" "nes_video.c" "nes_input.c" "nes_sound.c" "bruce_stdio.c" ${NOFRENDO_SOURCES}
    INCLUDE_DIRS "." "${NOFRENDO_DIR}" "../../../include" "../../../../src")

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
    # The project builds with CONFIG_COMPILER_OPTIMIZATION_SIZE (-Os)
    # globally. That's the wrong tradeoff for the nofrendo sources bundled
    # into this component: on-device measurement (nes_video.c's perf report)
    # shows CPU+PPU emulation alone -- the 6502 core and PPU renderer, not
    # this app's video blit or audio write, both timed separately and already
    # accounted for -- taking longer per frame than the entire NTSC frame
    # budget by itself. That's exactly the kind of tight, hot-loop code -Os
    # sacrifices speed on to save flash. A later -O flag on the compiler
    # command line wins over an earlier one, and target_compile_options here
    # is emitted after the project-wide -Os (see compile_commands.json), so
    # this overrides it for every source in this component -- including
    # main.c/nes_osd.c/etc, not just the nofrendo sources, since there's no
    # per-file granularity available through idf_component_register(), but
    # those files are cheap OSD glue, not hot loops, so the flash-size cost
    # of optimizing them for speed too is negligible. Trades some flash size
    # back for the speed the emulation core actually needs, without touching
    # a single line of emulation logic.
    -O2)
