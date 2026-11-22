# Compiles an app component against the firmware's LVGL (core_sdk/lvgl.h):
# LVGL's headers and Bruce's lv_conf.h only. The code itself is in the
# firmware, so an app must not depend on the lvgl component. LVGL's headers
# come from the firmware build's managed_components/ (build the firmware once).
#
#     include(${CMAKE_CURRENT_LIST_DIR}/../../../lvgl.cmake)   # from main/CMakeLists.txt
#     bruce_use_lvgl(${COMPONENT_LIB})
set(BRUCE_REPO_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

function(bruce_use_lvgl target)
    set(lvgl_dir "${BRUCE_REPO_DIR}/managed_components/lvgl__lvgl")
    if(NOT EXISTS "${lvgl_dir}/lvgl.h")
        message(FATAL_ERROR "LVGL headers not found in ${lvgl_dir}; build the firmware once to download them")
    endif()
    target_include_directories(${target} PRIVATE "${lvgl_dir}" "${BRUCE_REPO_DIR}/src/core_sdk/lvgl")
    target_compile_definitions(${target} PRIVATE LV_KCONFIG_IGNORE)
endfunction()
