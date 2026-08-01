if(CMAKE_SCRIPT_MODE_FILE)
    idf_component_register(REQUIRES mbedtls lwip)
    return()
endif()

include(FetchContent)

# Temporary in-tree ESP-IDF wrapper. Move this component to the maintained
# libssh2_esp fork once that repository is available.
set(LIBSSH2_UPSTREAM_COMMIT "74bb307bd52a26c8b579ed0e0b5c197388bb8a46")
FetchContent_Declare(libssh2_upstream
    GIT_REPOSITORY https://github.com/vszakats/libssh2.git
    GIT_TAG ${LIBSSH2_UPSTREAM_COMMIT}
    GIT_SHALLOW FALSE
    PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR>
                  -P "${CMAKE_CURRENT_LIST_DIR}/patch_max_packet_len.cmake")

cmake_policy(PUSH)
cmake_policy(SET CMP0169 OLD)
FetchContent_GetProperties(libssh2_upstream)
if(NOT libssh2_upstream_POPULATED)
    FetchContent_Populate(libssh2_upstream)
endif()
cmake_policy(POP)

set(LIBSSH2_SOURCE_DIR "${libssh2_upstream_SOURCE_DIR}")
set(LIBSSH2_SOURCES
    "${LIBSSH2_SOURCE_DIR}/src/agent.c"
    "${LIBSSH2_SOURCE_DIR}/src/bcrypt_pbkdf.c"
    "${LIBSSH2_SOURCE_DIR}/src/chacha.c"
    "${LIBSSH2_SOURCE_DIR}/src/channel.c"
    "${LIBSSH2_SOURCE_DIR}/src/cipher-chachapoly.c"
    "${LIBSSH2_SOURCE_DIR}/src/comp.c"
    "${LIBSSH2_SOURCE_DIR}/src/crypt.c"
    "${LIBSSH2_SOURCE_DIR}/src/global.c"
    "${LIBSSH2_SOURCE_DIR}/src/hostkey.c"
    "${LIBSSH2_SOURCE_DIR}/src/keepalive.c"
    "${LIBSSH2_SOURCE_DIR}/src/kex.c"
    "${LIBSSH2_SOURCE_DIR}/src/knownhost.c"
    "${LIBSSH2_SOURCE_DIR}/src/mac.c"
    "${LIBSSH2_SOURCE_DIR}/src/mbedtls.c"
    "${LIBSSH2_SOURCE_DIR}/src/misc.c"
    "${LIBSSH2_SOURCE_DIR}/src/packet.c"
    "${LIBSSH2_SOURCE_DIR}/src/pem.c"
    "${LIBSSH2_SOURCE_DIR}/src/poly1305.c"
    "${LIBSSH2_SOURCE_DIR}/src/publickey.c"
    "${LIBSSH2_SOURCE_DIR}/src/scp.c"
    "${LIBSSH2_SOURCE_DIR}/src/session.c"
    "${LIBSSH2_SOURCE_DIR}/src/sftp.c"
    "${LIBSSH2_SOURCE_DIR}/src/transport.c"
    "${LIBSSH2_SOURCE_DIR}/src/userauth.c"
    "${LIBSSH2_SOURCE_DIR}/src/userauth_kbd_packet.c"
    "${LIBSSH2_SOURCE_DIR}/src/version.c")

idf_component_register(
    SRCS ${LIBSSH2_SOURCES}
    INCLUDE_DIRS "${LIBSSH2_SOURCE_DIR}/include"
    PRIV_INCLUDE_DIRS "." "${LIBSSH2_SOURCE_DIR}/src"
    REQUIRES mbedtls lwip)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
    LIBSSH2_MBEDTLS)

# Upstream is warning-clean on its own CI matrix, but ESP-IDF enables a few
# toolchain-specific diagnostics globally.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include lwip/sockets.h
    -Wno-error=format
    -Wno-error=maybe-uninitialized
    -Wno-error=uninitialized)
