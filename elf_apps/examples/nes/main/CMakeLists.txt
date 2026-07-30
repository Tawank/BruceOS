include("${CMAKE_CURRENT_LIST_DIR}/../../../../components/nofrendo/nofrendo_sources.cmake")

# project_elf() links only libmain.a, so compile the reusable component sources
# into this archive instead of relying on an ordinary component dependency.
idf_component_register(
    SRCS "main.c" "nes_osd.c" "bruce_stdio.c" ${NOFRENDO_SOURCES}
    INCLUDE_DIRS "." "${NOFRENDO_DIR}" "../../../include" "../../../../src")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=attributes
    -Wno-error=format
    -Wno-error=implicit-fallthrough
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-to-pointer-cast
    -Wno-error=nonnull-compare
    -Wno-error=pointer-to-int-cast
    -Wno-error=unused-const-variable
    -Wno-unused-function)
