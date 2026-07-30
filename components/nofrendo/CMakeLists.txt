include("${CMAKE_CURRENT_LIST_DIR}/nofrendo_sources.cmake")

idf_component_register(
    SRCS ${NOFRENDO_SOURCES}
    INCLUDE_DIRS "${NOFRENDO_DIR}")

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=attributes
    -Wno-error=format
    -Wno-error=implicit-fallthrough
    -Wno-error=incompatible-pointer-types
    -Wno-error=int-to-pointer-cast
    -Wno-error=pointer-to-int-cast
    -Wno-error=unused-const-variable)
