idf_component_register(
    SRCS "args.c"
    INCLUDE_DIRS "."
)

# args is Bruce-specific and uses only the public Core SDK surface. Keep the
# include private so consumers see only args.h.
target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/src")
