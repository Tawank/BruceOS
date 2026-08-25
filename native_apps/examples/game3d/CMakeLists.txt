cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(game3d)

# project_elf() links only "main" (libmain.a) into the final ELF by default
# (see components/elf_loader/elf_loader.cmake) -- ELF_COMPONENTS is its
# extension point for pulling in additional components' object code, needed
# here because Jet (the 3D rasteriser this app renders with -- see
# main/idf_component.yml and README.md) builds as its own ordinary
# component "jet", not as sources glob'd into main.
list(APPEND ELF_COMPONENTS "jet")

include(elf_loader)
project_elf(game3d)
