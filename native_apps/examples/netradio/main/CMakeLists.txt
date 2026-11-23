idf_component_register(
    SRCS "netradio.c" "stations.c" "finder.c" "json.c"
    INCLUDE_DIRS "." "../../../include" "../../../../src")
include(${CMAKE_CURRENT_LIST_DIR}/../../../lvgl.cmake)
bruce_use_lvgl(${COMPONENT_LIB})
