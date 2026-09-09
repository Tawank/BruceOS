cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(loader_selftest)

include(elf_loader)
project_elf(loader_selftest)
