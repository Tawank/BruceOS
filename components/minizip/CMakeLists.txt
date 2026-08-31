# Vendored from zlib's own contrib/minizip (Info-Zip license, see
# LICENSE.Info-Zip) - unmodified upstream source, just the container-format
# pieces (zip.c/unzip.c/ioapi.c), not the CLI tools (minizip.c/miniunz.c) or
# the Windows-only iowin32.c. core/archive/archive_zip.c always opens with
# its own storage-backed zlib_filefunc64_def (see ioapi.h) instead of
# ioapi.c's default stdio FILE* one (fill_fopen64_filefunc() is never
# called) - but zip.c/unzip.c still unconditionally call ioapi.c's
# ZOPEN64()/ZSEEK64()/ZTELL64() bridge helpers regardless of which
# zlib_filefunc64_def backs them, so ioapi.c itself can't be dropped from
# the build. It's kept, with MINIZIP_FOPEN_NO_64 defined below to steer its
# unused stdio fallback path onto plain fopen()/ftello()/fseeko() instead of
# the fopen64()/ftello64()/fseeko64() large-file variants, which don't exist
# on ESP-IDF's newlib toolchain.
idf_component_register(SRCS "zip.c" "unzip.c" "ioapi.c"
                       INCLUDE_DIRS "."
                       REQUIRES zlib)

target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIZIP_FOPEN_NO_64)
