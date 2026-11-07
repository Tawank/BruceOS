idf_component_register(
    SRCS "cmatrix.c"
    INCLUDE_DIRS "." "../../../include" "../../../../src"
                 "../../../../components/ncurses/include" "../../../../components/ncurses/ncurses")

# cmatrix.c is adapted from upstream cmatrix (GPLv3, see its own top
# comment for exactly what changed and why), not vendored verbatim --
# it compiles warning-clean with -Wall -Wextra on host gcc, but the
# ESP-IDF toolchain's own default warning set has repeatedly caught
# real issues host gcc didn't during this port's development (see
# components/ncurses/README.md); -Wno-error=sign-compare is kept here
# defensively for the signed/unsigned char comparisons this file's
# rendering logic does throughout, matching doom's/vi's own equivalent
# lists for the same reason.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=sign-compare
    -O2)
