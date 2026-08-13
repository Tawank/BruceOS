# Bruce ELF SDK

This directory provides the public SDK surface and build tooling for Bruce
ELF applications.

## Layout

- `include/bruce_sdk.h` pulls in the runtime/loader/manifest/process/memory/permission/result/storage/display
  public Core SDK APIs and the `BRUCE_APP_MANIFEST()` macro.  Apps that need
  `config`, `dialog`, `display`, `http`, `input`, `stdio`, or `wifi` should also
  include the corresponding `core_sdk/*.h` headers.
- `elf_apps/examples/elf_loader/` — template for a loader ELF app (`elf_loader.elf`).
- `elf_apps/examples/game/` — template for a simple ELF app (`game.elf`).
- `elf_apps/examples/nes/` — Nofrendo NES emulator port using Bruce display, input, and storage APIs.
- `components/nofrendo/` — reusable ESP-IDF component containing the emulator core.
- `tools/build_elf_apps.py` — builds the templates and injects the
  `.bruce.manifest` section from each app's `manifest.json`.

## Building

Set `IDF_PATH` and run:

```bash
python3 elf_apps/tools/build_elf_apps.py --target esp32s3
```

By default the script builds every example. Use `--app` to build a subset; the
option may be repeated:

```bash
python3 elf_apps/tools/build_elf_apps.py --target esp32s3 --app game
python3 elf_apps/tools/build_elf_apps.py --target esp32s3 --app game --app nes
```

Final ELF files are written to:

- `elf_apps/examples/elf_loader.elf`
- `elf_apps/examples/game.elf`
- `elf_apps/examples/nes.elf`

Nofrendo can be launched with a ROM path or without one to open the file picker:

```
elf ./nes.elf /roms/game.nes
elf ./nes.elf
```

The Back input exits. D-pad/gamepad inputs are mapped directly; keyboard controls
are WASD, J (A), K (B), Enter (Start), and Space (Select). Audio is currently
disabled because the public Bruce ELF SDK does not expose an audio stream API.
The NES project builds with `-Os` because both executable memory and internal
RAM are constrained on non-PSRAM devices.

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

GUI applications should query their viewport every render loop because a
background application may be tiled or hidden. Hidden applications receive
zero dimensions. Present complete frames explicitly:

```c
if (display__width() > 0 && display__begin_frame() == BRUCE_OK) {
    display__fill_screen(BRUCE_COLOR_BLACK);
    display__set_cursor(2, 2);
    display__print("Hello");
    display__present();
}
```

Use `display__begin_frame()` and `display__present()` around each frame. Tile layout
management is launcher-only and is not exported to ELF applications.

ELF applications may call the unrestricted `notification__push()` and
`notification__dismiss()` APIs. They may also publish, remove, and list global
1bpp status icons with `status_icon__push()`, `status_icon__remove()`, and
`status_icon__list()`. Status icons are runtime-only and are rendered by the
launcher or by applications, not by Core.

Any ELF application may also create its own overlay: a small always-on-top
drawing surface composited into every flush independently of the normal
foreground/tile system, so drawing into it never contends with whatever else
is on screen. This is what `notification__push()` itself is built on (see
`src/modules/notification_service/notification_service.c`) -- an app that wants a
different notification UI, a HUD, or a floating menu can just use the same
primitive directly instead:

```c
bruce_display_overlay_id_t overlay;
display__overlay_create(display__screen_width() - 60, 4, 56, 16, &overlay);
display__overlay_begin(overlay);
display__fill_rect(0, 0, 56, 16, BRUCE_COLOR_NAVY);
display__draw_string("HUD", 4, 4);
display__overlay_end(overlay);
display__overlay_show(overlay);
/* ... display__overlay_move()/hide()/show() as needed ... */
display__overlay_destroy(overlay); /* also happens automatically on exit */
```

## ESP-IDF v6 compatibility note

`espressif/elf_loader` v1.3.1 originally required a small patch to build against
ESP-IDF v6.0.2.  The current build setup no longer relies on a separate
`tools/apply_patches.sh` or `patches/` directory; the managed component builds
as configured by this project.
