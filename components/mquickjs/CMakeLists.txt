idf_component_register(
    SRCS
        "mquickjs.c"
        "cutils.c"
        "dtoa.c"
        "libm.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ""
)

# ---------------------------------------------------------------------------
# Build-time generation of the Micro QuickJS standard library headers.
# The stdlib is generated from src/modules/loaders/js/mqjs_stdlib.c (the
# global object definition) and mquickjs_build.c (the host build tool).  The
# output is always forced to 32-bit target format (matching ESP32) via the
# generator's -m32 flag.  Generated headers are placed next to the stdlib
# source under src/modules/loaders/js/.
# ---------------------------------------------------------------------------

set(MQJS_STDLIB_DIR "${CMAKE_SOURCE_DIR}/src/modules/loaders/js")
set(MQJS_STDLIB_H "${MQJS_STDLIB_DIR}/mqjs_stdlib.h")
set(MQJS_ATOM_H   "${MQJS_STDLIB_DIR}/mquickjs_atom.h")
set(MQJS_BUILD_HOST "${CMAKE_CURRENT_BINARY_DIR}/mqjs_build_host")

find_program(MQJS_HOST_CC gcc DOC "Host C compiler for mquickjs stdlib generator")
if(NOT MQJS_HOST_CC)
    find_program(MQJS_HOST_CC cc)
endif()
if(NOT MQJS_HOST_CC)
    message(FATAL_ERROR "No host C compiler (gcc/cc) found; cannot generate mquickjs stdlib headers.")
endif()

add_custom_command(
    OUTPUT ${MQJS_STDLIB_H} ${MQJS_ATOM_H}
    # Force the host compiler to use the system assembler/linker instead of
    # any target-specific toolchain (e.g. esp32ulp-elf) that may appear first
    # in PATH when the ESP-IDF environment is active.
    COMMAND ${CMAKE_COMMAND} -E env "PATH=/usr/bin:$ENV{PATH}"
            ${MQJS_HOST_CC} -O2 -I${CMAKE_CURRENT_SOURCE_DIR}
            -o ${MQJS_BUILD_HOST}
            ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_build.c
            ${MQJS_STDLIB_DIR}/mqjs_stdlib.c
            -lm
    COMMAND ${MQJS_BUILD_HOST} -m32 > ${MQJS_STDLIB_H}
    COMMAND ${MQJS_BUILD_HOST} -m32 -a > ${MQJS_ATOM_H}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_build.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_build.h
        ${MQJS_STDLIB_DIR}/mqjs_stdlib.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs.h
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_priv.h
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_opcode.h
        ${CMAKE_CURRENT_SOURCE_DIR}/cutils.h
        ${CMAKE_CURRENT_SOURCE_DIR}/dtoa.h
        ${CMAKE_CURRENT_SOURCE_DIR}/libm.h
        ${CMAKE_CURRENT_SOURCE_DIR}/list.h
    COMMENT "Generating mquickjs stdlib headers (32-bit target)"
    VERBATIM
)

add_custom_target(mquickjs_generate_stdlib DEPENDS ${MQJS_STDLIB_H} ${MQJS_ATOM_H})

# Make the main component build wait for the generated headers.
add_dependencies(${COMPONENT_LIB} mquickjs_generate_stdlib)

# Expose the generated headers to mquickjs.c (mquickjs_atom.h) and to the
# JS loader module (mqjs_stdlib.h).
target_include_directories(${COMPONENT_LIB} PUBLIC ${MQJS_STDLIB_DIR})

# Propagate a dependency on the generated headers to any target that links
# against mquickjs (e.g., the JS loader).  This ensures the generator runs
# before source files that include these headers are compiled.
set_property(TARGET ${COMPONENT_LIB} APPEND PROPERTY
             INTERFACE_SOURCES ${MQJS_STDLIB_H} ${MQJS_ATOM_H})

# Silence warnings in the upstream engine files so the build stays clean.
set_source_files_properties(
    mquickjs.c cutils.c dtoa.c libm.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-variable -Wno-unused-label -Wno-comment -Wno-unused-function -Wno-maybe-uninitialized -Wno-conversion -Wno-type-limits -Wno-format -Wno-strict-aliasing -Wno-dangling-pointer"
)
