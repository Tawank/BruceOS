# Patches the vendored libssh2's MAX_SSH_PACKET_LEN down from the
# RFC4253-mandated 35000 bytes. LIBSSH2_SESSION embeds two buffers of this
# size directly by value (struct transportpacket's buf/outbuf), so at 35000
# that one struct requires a single ~70KB+ contiguous allocation. On
# PSRAM-less ESP32-class boards running WiFi/BLE/display alongside the rest
# of BruceOS, the heap rarely has a single free block that large even when
# total free memory looks comfortable. 8192 bytes is ample for an
# interactive shell/terminal SSH session and shrinks the struct to a small
# fraction of that.
set(header "${SOURCE_DIR}/src/libssh2_priv.h")
file(READ "${header}" contents)
string(FIND "${contents}" "#define MAX_SSH_PACKET_LEN 35000" found_at)
if(found_at EQUAL -1)
    message(FATAL_ERROR
        "libssh2_esp patch: '#define MAX_SSH_PACKET_LEN 35000' not found in "
        "${header}; upstream may have changed this line, update "
        "patch_max_packet_len.cmake to match.")
endif()
string(REPLACE
    "#define MAX_SSH_PACKET_LEN 35000"
    "#define MAX_SSH_PACKET_LEN 8192 /* reduced from 35000 by BruceOS, see components/libssh2_esp/patch_max_packet_len.cmake */"
    contents "${contents}")
file(WRITE "${header}" "${contents}")
