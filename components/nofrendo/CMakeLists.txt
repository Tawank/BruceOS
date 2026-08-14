include("${CMAKE_CURRENT_LIST_DIR}/nofrendo_sources.cmake")

idf_component_register(
    SRCS ${NOFRENDO_SOURCES}
    INCLUDE_DIRS "${NOFRENDO_DIR}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=attributes
    -Wno-error=format
    -Wno-error=implicit-fallthrough
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-to-pointer-cast
    -Wno-error=pointer-to-int-cast
    -Wno-error=stringop-truncation
    -Wno-error=unused-const-variable
    # See the matching -O2 override (and rationale) in
    # native_apps/examples/nes/main/CMakeLists.txt: that's where these sources
    # actually get compiled for the nes ELF app (project_elf() only links
    # libmain.a, so the app's own main component compiles NOFRENDO_SOURCES
    # directly instead of depending on this component) -- this CMakeLists.txt
    # is only exercised by some other, non-ELF consumer of the nofrendo
    # component, if one exists. Mirrored here for consistency in case one
    # ever needs it.
    -O2)
