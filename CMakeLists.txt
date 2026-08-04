# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS src)

# Keep each board's (and the emulator's) sdkconfig isolated from the others.
# ESP-IDF's project.cmake always resolves SDKCONFIG to
# ${CMAKE_SOURCE_DIR}/sdkconfig regardless of -B, so every non-default build
# dir must redirect it explicitly to get its own persistent config. Build
# directory naming convention: build-qemu for the QEMU test image,
# build-board/<board> for a board under boards/<board>/sdkconfig.defaults
# (nested under one build-board/ parent so it's a single .gitignore entry;
# see tools/board.py, which drives this from board ids).
get_filename_component(BRUCE_BUILD_DIR_NAME "${CMAKE_BINARY_DIR}" NAME)
get_filename_component(BRUCE_BUILD_DIR_PARENT "${CMAKE_BINARY_DIR}" DIRECTORY)
get_filename_component(BRUCE_BUILD_DIR_PARENT_NAME "${BRUCE_BUILD_DIR_PARENT}" NAME)
if(BRUCE_BUILD_DIR_NAME STREQUAL "build-qemu")
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.qemu")
elseif(BRUCE_BUILD_DIR_PARENT_NAME STREQUAL "build-board")
    set(BRUCE_BOARD_ID "${BRUCE_BUILD_DIR_NAME}")
    set(BRUCE_BOARD_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/boards/${BRUCE_BOARD_ID}/sdkconfig.defaults")
    if(EXISTS "${BRUCE_BOARD_DEFAULTS}")
        set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;boards/${BRUCE_BOARD_ID}/sdkconfig.defaults")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BruceIDF)
