idf_component_register(
    SRCS "main.c" "busybox_shim.c" "vi.c"
    INCLUDE_DIRS "." "../../../include" "../../../../src")

# BusyBox's vi.c is vendored in this directory (see libbb.h's own comment
# for why the surrounding libbb.h/config.h are a from-scratch, hand-written
# stand-in for BusyBox's real Kbuild-generated ones, not a fork of either).
# Same rationale as doom's equivalent list:
# these are long-standing, harmless patterns in upstream vi.c against any
# sufficiently strict compiler, not anything introduced by vendoring it here.
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
