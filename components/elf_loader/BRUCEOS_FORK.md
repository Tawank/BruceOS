# BruceOS ELF loader fork

This component was vendored from `Tawank/elf_loader` commit
`8dcfc9aca1220c401aabf121968a4996b3e5600d`.

BruceOS adds a size-checked `esp_elf_relocate_xip()` mode. On bus-mirrored
targets it relocates `.text` and `.rodata` through writable work buffers using
their final flash-mapped addresses, commits those sections through caller-owned
storage callbacks, and retains the mapping until `esp_elf_deinit()`. Writable
sections continue to use RAM. The original `esp_elf_relocate()` API remains
RAM-backed.
