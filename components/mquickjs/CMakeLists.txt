idf_component_register(
    SRCS
        "mquickjs.c"
        "cutils.c"
        "dtoa.c"
        "libm.c"
        "js_port.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ""
)

# ---------------------------------------------------------------------------
# Build-time generation of the Micro QuickJS standard library headers.
# The stdlib is generated from mqjs_stdlib.c (the global object definition)
# and mquickjs_build.c (the host build tool).  The output is always forced to
# 32-bit target format (matching ESP32) via the generator's -m32 flag.
# ---------------------------------------------------------------------------

set(MQJS_STDLIB_H "${CMAKE_CURRENT_SOURCE_DIR}/mqjs_stdlib.h")
set(MQJS_ATOM_H   "${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_atom.h")
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
    COMMAND ${MQJS_HOST_CC} -O2 -I${CMAKE_CURRENT_SOURCE_DIR}
            -o ${MQJS_BUILD_HOST}
            ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_build.c
            ${CMAKE_CURRENT_SOURCE_DIR}/mqjs_stdlib.c
            -lm
    COMMAND ${MQJS_BUILD_HOST} -m32 > ${MQJS_STDLIB_H}
    COMMAND ${MQJS_BUILD_HOST} -m32 -a > ${MQJS_ATOM_H}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_build.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mquickjs_build.h
        ${CMAKE_CURRENT_SOURCE_DIR}/mqjs_stdlib.c
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

# Silence warnings in the upstream engine files so the build stays clean.
set_source_files_properties(
    mquickjs.c cutils.c dtoa.c libm.c
    PROPERTIES COMPILE_FLAGS "-Wno-unused-variable -Wno-unused-label -Wno-comment -Wno-unused-function -Wno-maybe-uninitialized -Wno-conversion -Wno-type-limits -Wno-format -Wno-strict-aliasing"
)
