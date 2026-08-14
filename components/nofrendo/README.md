# Nofrendo ESP-IDF component

This component contains the portable Nofrendo NES emulator core from
`Tawank/arduino-nofrendo`. Hardware and operating-system integration is supplied
by the consumer through the functions declared in `osd.h`.

Normal ESP-IDF applications can add `nofrendo` to their component `REQUIRES`.
Bruce ELF applications must instead include `nofrendo_sources.cmake` and add
`${NOFRENDO_SOURCES}` to their main component. Espressif's `project_elf()` target
links only `libmain.a`, so a regular component dependency would not be included
in the loadable ELF.

The Bruce implementation is in `native_apps/examples/nes/main/nes_osd.c` and uses
only public `core_sdk` APIs for display, input, storage, memory, and timing.
