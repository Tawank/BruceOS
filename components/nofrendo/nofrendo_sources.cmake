set(NOFRENDO_DIR "${CMAKE_CURRENT_LIST_DIR}/src")

file(GLOB NOFRENDO_SOURCES
     "${NOFRENDO_DIR}/*.c"
     "${NOFRENDO_DIR}/cpu/*.c"
     "${NOFRENDO_DIR}/libsnss/*.c"
     "${NOFRENDO_DIR}/mappers/*.c"
     "${NOFRENDO_DIR}/nes/*.c"
     "${NOFRENDO_DIR}/sndhrdw/*.c")

# The platform frontend owns configuration. The desktop parser relies on host
# stdio behavior and is not needed by embedded integrations.
list(REMOVE_ITEM NOFRENDO_SOURCES "${NOFRENDO_DIR}/config.c")
