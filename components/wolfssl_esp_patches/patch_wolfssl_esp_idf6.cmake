# Patches four small upstream bugs in the managed wolfssl/wolfssl component
# (fetched into managed_components/wolfssl__wolfssl by the ESP-IDF component
# manager) that otherwise break the build against ESP-IDF v6 on this
# PSRAM-less, ESP32-S3, software-crypto-only configuration
# (CONFIG_ESP_WOLFSSL_NO_ESP32_CRYPT=y, see sdkconfig.defaults):
#
# 1-3. wc_ShaFree()/wc_Sha256Free()/wc_Sha512Free()/wc_Sha384Free() guard their
#      ESP32 hardware-crypto cleanup call with `defined(WOLFSSL_ESP32)`, but
#      every OTHER ESP32-crypto guard in these same files (correctly) checks
#      `defined(WOLFSSL_ESP32_CRYPT)` -- the flag that's actually unset when
#      hardware crypto is disabled. `WOLFSSL_ESP32` alone stays defined
#      regardless (it just means "this chip family", not "use HW crypto"),
#      so with HW crypto disabled these four functions still reference the
#      HW-only `->ctx` struct member and a HW-only helper that no longer
#      compiles. Fix: match the correct macro these files use everywhere else.
#
# 4.   esp_sdk_mem_lib.c's `enum sdk_memory_segment` declares a member named
#      `thread_local` (line 2 of the enum). Newer libc headers pulled in by
#      ESP-IDF v6's toolchain define `thread_local` as a macro (C11's
#      <threads.h> convention), which corrupts the enum declaration itself
#      and cascades into "SDK_MEMORY_SEGMENT_COUNT undeclared" and similar
#      errors for every other enumerator. Fix: rename just that one
#      enumerator (its only 2 references) to a name that isn't a reserved
#      macro. This file's actual load-bearing content -- wc_debug_pvPortMalloc/
#      wc_debug_vPortFree, referenced by wolfssl/wolfcrypt/settings.h's XMALLOC
#      macro whenever DEBUG_WOLFSSL_MALLOC is set (unconditionally true in the
#      generated user_settings.h) -- must stay compiled in; excluding the
#      whole file breaks the final link instead of the compile.
set(wolfssl_dir "${WOLFSSL_COMPONENT_DIR}")

function(patch_wolfssl_file relative_path search replace)
    set(file "${wolfssl_dir}/${relative_path}")
    file(READ "${file}" contents)
    string(FIND "${contents}" "${replace}" already_patched)
    if(NOT already_patched EQUAL -1)
        return() # idempotent: already applied (e.g. unchanged managed_components cache)
    endif()
    string(FIND "${contents}" "${search}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR
            "wolfssl_esp_patches: expected text not found in ${file}; "
            "upstream wolfssl may have changed, update patch_wolfssl_esp_idf6.cmake to match.\n"
            "Expected:\n${search}")
    endif()
    string(REPLACE "${search}" "${replace}" contents "${contents}")
    file(WRITE "${file}" "${contents}")
endfunction()

patch_wolfssl_file("wolfcrypt/src/sha.c"
    "void wc_ShaFree(wc_Sha* sha)
{
    if (sha == NULL)
        return;

#if defined(WOLFSSL_ESP32) &&  !defined(NO_WOLFSSL_ESP32_CRYPT_HASH)"
    "void wc_ShaFree(wc_Sha* sha)
{
    if (sha == NULL)
        return;

#if defined(WOLFSSL_ESP32_CRYPT) &&  !defined(NO_WOLFSSL_ESP32_CRYPT_HASH)")

patch_wolfssl_file("wolfcrypt/src/sha256.c"
    "void wc_Sha256Free(wc_Sha256* sha256)
{
    if (sha256 == NULL)
        return;

#if defined(WOLFSSL_ESP32) && \\"
    "void wc_Sha256Free(wc_Sha256* sha256)
{
    if (sha256 == NULL)
        return;

#if defined(WOLFSSL_ESP32_CRYPT) && \\")

patch_wolfssl_file("wolfcrypt/src/sha512.c"
    "void wc_Sha512Free(wc_Sha512* sha512)
{
    if (sha512 == NULL)
        return;

#if defined(WOLFSSL_ESP32) && \\"
    "void wc_Sha512Free(wc_Sha512* sha512)
{
    if (sha512 == NULL)
        return;

#if defined(WOLFSSL_ESP32_CRYPT) && \\")

patch_wolfssl_file("wolfcrypt/src/sha512.c"
    "void wc_Sha384Free(wc_Sha384* sha384)
{
    if (sha384 == NULL)
        return;

#if defined(WOLFSSL_ESP32) && !defined(NO_WOLFSSL_ESP32_CRYPT_HASH)  && \\"
    "void wc_Sha384Free(wc_Sha384* sha384)
{
    if (sha384 == NULL)
        return;

#if defined(WOLFSSL_ESP32_CRYPT) && !defined(NO_WOLFSSL_ESP32_CRYPT_HASH)  && \\")

patch_wolfssl_file("wolfcrypt/src/port/Espressif/esp_sdk_mem_lib.c"
    "    mem_map_io = 0,
    thread_local,"
    "    mem_map_io = 0,
    wc_thread_local,")

patch_wolfssl_file("wolfcrypt/src/port/Espressif/esp_sdk_mem_lib.c"
    "    sdk_log_meminfo(thread_local,  _thread_local_start, _thread_local_end);"
    "    sdk_log_meminfo(wc_thread_local,  _thread_local_start, _thread_local_end);")
