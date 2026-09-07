# Unlike doomgeneric (a whole multi-file engine, see ../../doom/main/
# doomgeneric_sources.cmake), this app needs exactly one upstream file --
# BusyBox's editors/vi.c -- so a full git clone of the busybox monorepo
# would be a lot of needless weight just to reach it. GitHub's raw-content
# endpoint serves a single file at a pinned commit directly, so that's
# fetched instead, with EXPECTED_HASH pinning its exact content the same
# way the commit pin does for doomgeneric's git checkout.
#
# Mirror note: BusyBox's own canonical host, git.busybox.net, wasn't
# reachable from the sandbox this port was developed in (connection just
# hung) -- github.com/mirror/busybox, a long-standing read-only mirror of
# the same history, is pinned here instead because it's the one this was
# actually verified against end-to-end. If git.busybox.net works fine in
# your own build environment, pointing BUSYBOX_RAW_BASE at
# "https://git.busybox.net/busybox/plain" (same path shape) instead should
# work as a drop-in replacement -- untested here, but there's no reason
# upstream's own copy of the same commit would differ.
set(BUSYBOX_UPSTREAM_COMMIT "371fe9f71d445d18be28c82a2a6d82115c8af19d")
set(BUSYBOX_RAW_BASE "https://raw.githubusercontent.com/mirror/busybox/${BUSYBOX_UPSTREAM_COMMIT}")
set(VI_UPSTREAM_C "${CMAKE_BINARY_DIR}/busybox_vi_upstream/vi.c")

if(NOT EXISTS "${VI_UPSTREAM_C}")
    file(DOWNLOAD "${BUSYBOX_RAW_BASE}/editors/vi.c" "${VI_UPSTREAM_C}"
        EXPECTED_HASH SHA256=3bd97f49572a3815ca85801495d98dca61caa66816ee7418724641699c5a577e
        STATUS VI_DOWNLOAD_STATUS)
    list(GET VI_DOWNLOAD_STATUS 0 VI_DOWNLOAD_CODE)
    if(NOT VI_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE "${VI_UPSTREAM_C}")
        list(GET VI_DOWNLOAD_STATUS 1 VI_DOWNLOAD_MESSAGE)
        message(FATAL_ERROR "Failed to download BusyBox vi.c: ${VI_DOWNLOAD_MESSAGE}")
    endif()
endif()
