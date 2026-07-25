# Bruce ELF SDK

This directory provides the public SDK surface and build tooling for Bruce
ELF applications.

## Layout

- `include/bruce_sdk.h` — pulls in the runtime/loader/manifest/task/memory/permission/result/storage
  public Core SDK APIs and the `BRUCE_APP_MANIFEST()` macro.  Apps that need
  `config`, `dialog`, `display`, `http`, `input`, `stdio`, or `wifi` should also
  include the corresponding `core_sdk/*.h` headers.
- `elf_apps/examples/elf_loader/` — template for a loader ELF app (`elf_loader.elf`).
- `elf_apps/examples/game/` — template for a simple ELF app (`game.elf`).
- `tools/build_elf_apps.py` — builds the templates and injects the
  `.bruce.manifest` section from each app's `manifest.json`.

## Building

Set `IDF_PATH` and run:

```bash
python3 elf_apps/tools/build_elf_apps.py --target esp32s3
```

Final ELF files are written to:

- `elf_apps/examples/elf_loader.elf`
- `elf_apps/examples/game.elf`

## Running

Copy the ELF files to a mounted path on the device, then from the terminal:

```
elf ./elf_loader.elf ./game.elf
```

The built-in `elf` command loads the named ELF file and passes the remaining
arguments to it.  `elf_loader.elf` receives `argv = ["elf_loader", "./game.elf"]`,
which it forwards to `app_runner__run_path()`.  Relative paths starting with
`./` are normalized to the root directory.  The `execute` permission is required
for `app_runner__run_path()`, so the manifest of `elf_loader.elf` requests it.

You can also load a single ELF directly:

```
elf ./game.elf
```

## Writing your own ELF app

1. Create a new ESP-IDF project.
2. Add `espressif/elf_loader` as a dependency.
3. Disable memory protection and the default symbol tables in
   `sdkconfig.defaults`:
   ```
   CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n
   CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n
   CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_LOCK=n
   CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_VIA_TEE=n
   CONFIG_ELF_LOADER_LIBC_SYMBOLS=n
   CONFIG_ELF_LOADER_ESPIDF_SYMBOLS=n
   ```
4. In `CMakeLists.txt`:
   ```cmake
   include(elf_loader)
   project(my_app)
   project_elf(my_app)
   ```
5. In `main/CMakeLists.txt`, add the SDK include path and the project `src/`
   directory so `core_sdk/...` headers can be found:
   ```cmake
   idf_component_register(SRCS "main.c"
                          INCLUDE_DIRS "path/to/sdk/include"
                                       "path/to/src")
   ```
6. Include `bruce_sdk.h` and export `int app_main(int argc, char **argv)`.
7. Provide a `manifest.json` in the project root (see the templates) and use
   `elf_apps/tools/build_elf_apps.py` as a reference for injecting the
   `.bruce.manifest` section after linking.

Alternatively, you can skip the external `manifest.json` and use the
`BRUCE_APP_MANIFEST(...)` macro once in your main file to embed the manifest
section at build time.

See `elf_apps/examples/` for complete examples.

## ESP-IDF v6 compatibility note

`espressif/elf_loader` v1.3.1 originally required a small patch to build against
ESP-IDF v6.0.2.  The current build setup no longer relies on a separate
`tools/apply_patches.sh` or `patches/` directory; the managed component builds
as configured by this project.
