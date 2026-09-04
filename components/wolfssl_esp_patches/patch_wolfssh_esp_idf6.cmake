# Patches one upstream wolfSSH design quirk in the managed wolfssl/wolfssh
# component (fetched into managed_components/wolfssl__wolfssh) that otherwise
# leaves PTY/terminal support silently compiled out on this build:
#
# wolfSSH's pty-req sender (SendChannelTerminalRequest / CreateMode, in
# src/internal.c) and the public wolfSSH_ChangeTerminalSize() (src/ssh.c) are
# guarded by `defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)`. NO_FILESYSTEM
# is unconditionally defined for every Espressif build in the shared
# managed_components/wolfssl__wolfssl/include/user_settings.h (this project's
# board has no local host terminal or file-based cert store, so that's the
# right default for wolfSSL's own file-loading APIs) -- but it also silently
# disables the entire terminal-request code path here, with no compile error:
# ssh__open_shell()/ssh__resize_pty() in core/ssh/ssh.c call
# wolfSSH_ChangeTerminalSize() unconditionally, which only surfaces as a link
# error ("undefined reference to wolfSSH_ChangeTerminalSize") once the rest of
# the build succeeds.
#
# CreateMode()'s NO_FILESYSTEM-gated code additionally has an inner branch
# that calls tcgetattr(STDIN_FILENO, ...) to mirror the *local* client's own
# terminal settings to the server -- meaningless on this board (no attached
# host tty), and not something ESP-IDF's newlib port necessarily supports
# correctly. NO_TERMIOS (an existing wolfSSH build knob, set alongside this
# patch in src/CMakeLists.txt) makes CreateMode take its "no termios, just
# report a fixed 38400 baud" branch instead, which is the correct behavior
# for a client with no local terminal to query.
#
# Fix: drop the `&& !defined(NO_FILESYSTEM)` half of the guard in the three
# call sites below, leaving `defined(WOLFSSH_TERM)` as the sole condition
# (matching how WOLFSSH_TERM is used everywhere else in this codebase).
#
# Un-gating SendChannelTerminalRequest() (above) also exposes it to this
# build's -Werror=maybe-uninitialized for the first time: `channel` is only
# ever read once `ret == WS_SUCCESS`, and it's only ever set to non-NULL in an
# earlier `if (ret == WS_SUCCESS) { channel = ChannelFind(...); ... }` block --
# genuinely safe, but GCC can't thread `ret`'s value across the two separate
# `if` statements. Silence it the same way as any other false-positive here:
# give `channel` a definite initial value.
set(wolfssh_dir "${WOLFSSH_COMPONENT_DIR}")

function(patch_wolfssh_file relative_path search replace)
    set(file "${wolfssh_dir}/${relative_path}")
    file(READ "${file}" contents)
    # NOTE: `replace` here is a strict prefix of `search`, so it is always
    # found in the *unpatched* text too -- check for `search` first (the
    # unpatched marker) and only fall back to the "already patched" check
    # when it's absent.
    string(FIND "${contents}" "${search}" found_at)
    if(NOT found_at EQUAL -1)
        string(REPLACE "${search}" "${replace}" contents "${contents}")
        file(WRITE "${file}" "${contents}")
        return()
    endif()
    string(FIND "${contents}" "${replace}" already_patched)
    if(NOT already_patched EQUAL -1)
        return() # idempotent: already applied (e.g. unchanged managed_components cache)
    endif()
    message(FATAL_ERROR
        "wolfssh_esp_patches: expected text not found in ${file}; "
        "upstream wolfssh may have changed, update patch_wolfssh_esp_idf6.cmake to match.\n"
        "Expected:\n${search}")
endfunction()

# Each file's two/one occurrences of this exact guard line are replaced in
# one pass (string(REPLACE) substitutes every match, not just the first).
patch_wolfssh_file("src/ssh.c"
    "#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)"
    "#if defined(WOLFSSH_TERM)")

patch_wolfssh_file("src/internal.c"
    "#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)"
    "#if defined(WOLFSSH_TERM)")

# wolfSSH's SFTP client (src/wolfsftp.c, enabled via WOLFSSH_SFTP in
# src/CMakeLists.txt for modules/ssh/ssh_sftp_app.c) is written with no
# NO_FILESYSTEM-avoidance at all -- unlike wolfSSL/wolfSSH's cert/key-loading
# helpers, its SFTP directory/file helpers (both the client Get/Put
# convenience wrappers and the server-side request handlers, all compiled
# into the same translation unit) call the real WFOPEN/WOPENDIR/WSTAT/etc.
# family unconditionally, which port.h only defines when NO_FILESYSTEM is
# *not* set. Left as-is, that's ~25 undefined-symbol/implicit-declaration
# errors, escalating to a link failure for the six ssh__sftp_* entry points
# this project actually calls (SFTP_connect/LS/Open/SendReadPacket/Close),
# even though none of the newly-compiled server/Get/Put code ever runs.
#
# wolfSSL's own NO_FILESYSTEM (managed_components/wolfssl__wolfssl/include/
# user_settings.h) exists to keep wolfSSL's cert/key loading off file paths
# -- this board loads keys from its own core/storage into memory instead --
# not because the board lacks real file I/O: core/storage/storage.c mounts
# a real littlefs partition via ESP-IDF's VFS (esp_vfs_littlefs_register()),
# and CONFIG_VFS_SUPPORT_DIR/CONFIG_VFS_SUPPORT_IO are on, so plain
# fopen()/opendir()/stat() work. Undoing NO_FILESYSTEM after user_settings.h
# is included, scoped to wolfSSH's own settings chain only (this file is
# wolfSSH-private, unlike the shared user_settings.h), lets port.h's normal
# stdio-backed macros compile instead -- wolfSSL's own object files are
# unaffected, since each component's sources are preprocessed independently.
# The only other `!defined(NO_FILESYSTEM)` guards this newly exposes are
# ssh.c's wolfSSH_ReadKey_file() (an unused file-based key-loading helper)
# and internal.c's SCP handler (dead anyway: WOLFSSH_SCP is never defined
# in this project) -- both inert, not called by anything in core/ssh/ssh.c
# or modules/ssh/*.
patch_wolfssh_file("wolfssh/settings.h"
    "#ifdef WOLFSSL_USER_SETTINGS
    #include \"user_settings.h\""
    "#ifdef WOLFSSL_USER_SETTINGS
    #include \"user_settings.h\"
    #ifdef WOLFSSH_SFTP
        #undef NO_FILESYSTEM
    #endif")

patch_wolfssh_file("src/internal.c"
    "int SendChannelTerminalRequest(WOLFSSH* ssh)
{
    byte* output;
    word32 idx;
    int ret = WS_SUCCESS;
    WOLFSSH_CHANNEL* channel;
    const char cType[] = \"pty-req\";"
    "int SendChannelTerminalRequest(WOLFSSH* ssh)
{
    byte* output;
    word32 idx;
    int ret = WS_SUCCESS;
    WOLFSSH_CHANNEL* channel = NULL;
    const char cType[] = \"pty-req\";")
