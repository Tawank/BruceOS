# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS src)

# Keep emulator settings isolated from the hardware sdkconfig.
get_filename_component(BRUCE_BUILD_DIR_NAME "${CMAKE_BINARY_DIR}" NAME)
if(BRUCE_BUILD_DIR_NAME STREQUAL "build-qemu")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.qemu")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BruceIDF)
