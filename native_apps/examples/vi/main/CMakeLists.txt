include("${CMAKE_CURRENT_LIST_DIR}/vi_sources.cmake")

idf_component_register(
    SRCS "main.c" "busybox_shim.c" "${VI_UPSTREAM_C}"
    INCLUDE_DIRS "." "../../../include" "../../../../src")

# BusyBox's vi.c is compiled here completely unmodified (see libbb.h's own
# comment for why: it's a from-scratch, hand-written stand-in for
# BusyBox's real Kbuild-generated libbb.h/config.h, not a fork of either --
# vi.c itself was never touched). Same rationale as doom's equivalent list:
# these are long-standing, harmless patterns in upstream vi.c against any
# sufficiently strict compiler, not anything introduced by fetching it here.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=sign-compare
    -Wno-error=type-limits
    -Wno-error=implicit-fallthrough
    -Wno-error=unused-but-set-variable
    -Wno-error=unused-variable
    -Wno-error=unused-function
    -Wno-error=unused-value
    -Wno-error=maybe-uninitialized
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=stringop-overflow
    -Wno-error=array-bounds
    -Wno-error=missing-field-initializers
    -O2)
