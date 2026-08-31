# Vendored from https://github.com/rxi/microtar (MIT license, see LICENSE) -
# unmodified upstream source. core/archive/archive.c supplies its own
# storage-backed mtar_t read/write/seek/close callbacks instead of using
# mtar_open()'s built-in stdio FILE* backend.
idf_component_register(SRCS "microtar.c"
                       INCLUDE_DIRS ".")
