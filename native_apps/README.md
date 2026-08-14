# Bruce External App SDK

This directory provides the public SDK surface and build tooling for Bruce
external applications.

## Layout

- `include/bruce_sdk.h` pulls in the args/runtime/loader/manifest/process/memory/permission/result/storage/display
  public Core SDK APIs and the `BRUCE_APP_MANIFEST()` macro.  Apps that need
  `config`, `dialog`, `display`, `http`, `input`, `stdio`, or `wifi` should also
  include the corresponding `core_sdk/*.h` headers.
- `native_apps/examples/elf_loader/` — template for a loader ELF app (`elf_loader.elf`).
- `native_apps/examples/game/` — template for a simple ELF app (`game.elf`).
- `native_apps/examples/nes/` — Nofrendo NES emulator port using Bruce display, input, and storage APIs.
- `components/nofrendo/` — reusable ESP-IDF component containing the emulator core.
- `tools/build_apps.py` — builds ELF or WASM apps from the same
  `examples/<app>/main/` sources and the same `manifest.json`.
- `tools/build_modules.py` — discovers build-enabled manifests below
  `src/modules/` and writes artifacts to `native_apps/build_modules/`.
- `wasm/` — private guest adapter, freestanding C runtime, and headers linked
  into WASM applications; these files are never compiled into firmware.

## Building

Set `IDF_PATH` and run:

```bash
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3
```

By default the script builds every example. Use `--app` to build a subset; the
option may be repeated:

```bash
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app game
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --app game --app nes
python3 native_apps/tools/build_modules.py --target wasm --module clock
```

Final ELF files are written to:

- `native_apps/examples/elf_loader.elf`
- `native_apps/examples/game.elf`
- `native_apps/examples/nes.elf`

WASM module output is written under `native_apps/build_modules/`, for example
`native_apps/build_modules/clock.wasm`. External examples are written beside
their native output when all APIs they use have bindings.

WASM builds require Clang with the `wasm32` target. Builds target the WebAssembly
MVP instruction set supported by the firmware's restricted WAMR configuration.
The build tools verify target support and fall back from ESP-IDF's target-limited
Clang to a system or versioned Clang when available. Use `--compiler` or
`WASM_CLANG` to select another LLVM installation explicitly.
The freestanding guest support under `native_apps/wasm/` supplies the standard C
subset used by supported apps, so no WASI sysroot is required. The post-link
validator rejects every WASI or `env` import and accepts only exact allowlisted
`bruce_sdk` function signatures.

## Building built-in modules as external apps

Modules opt into external builds by adding a `manifest.json` with a `build`
object. Discovery is recursive, so nested module directories are supported.
Build every compatible module or select one with `--module`:

```bash
python3 native_apps/tools/build_modules.py --list
python3 native_apps/tools/build_modules.py --target elf --idf-target esp32s3
python3 native_apps/tools/build_modules.py --target elf --module clock
python3 native_apps/tools/build_modules.py --target wasm --module my_module
```

The `build` object accepts source and include paths relative to the module
directory. `sources` is required; all other fields are optional:

```json
{
  "appName": "My module",
  "entryPoint": "my_module_main",
  "appIcon": "...base64 32x32 1bpp icon...",
  "coreAbiVersion": 4,
  "permissions": [],
  "stackSize": 8192,
  "build": {
    "name": "my_module",
    "sources": ["my_module_app.c"],
    "includeDirs": ["."],
    "compileDefinitions": ["FEATURE=1"],
    "compileOptions": ["-Os"],
    "componentDirs": ["../../../components/example_runtime"],
    "componentDependencies": ["example_runtime"],
    "linkOptions": ["-lm"],
    "sdkconfigDefaults": ["CONFIG_EXAMPLE_RUNTIME_FEATURE=y"],
    "targets": ["elf", "wasm"]
  }
}
```

`componentDirs`, `componentDependencies`, `linkOptions`, and
`sdkconfigDefaults` add local ESP-IDF components to ELF builds, statically link
their component archives and supporting libraries into the loadable image, and
configure those dependencies. They do not apply to WASM builds.

An ELF app that spawns a loader child with callbacks implemented inside its own
ELF must remain alive until that child exits. Returning from the parent entry
unloads its code and invalidates the child's entry, stop, and cleanup callbacks.

The tool generates disposable ESP-IDF project files below
`native_apps/build_modules/.work/` and writes final files such as
`native_apps/build_modules/clock.elf`. A module should only declare `wasm` when
every SDK API it uses is available from the WASM runtime.

The WASM target uses exactly the same sources and manifest as the ELF target.
The manifest's optional `entryPoint` (default `app_main`) is exported as WASM
`main`; no target-specific source directory is used. APIs used by an app must
therefore have equivalent ELF and WASM SDK implementations. The current
ESP-IDF/Nofrendo NES integration does not yet satisfy that requirement.

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

1. Create an app project with `main/` sources and a `manifest.json`.
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
6. Include `bruce_sdk.h` and export the manifest's `entryPoint` function,
   normally `int app_main(int argc, char **argv)`.
7. Provide a `manifest.json` in the project root (see the templates) and use
   `native_apps/tools/build_apps.py` as a reference for injecting the
   `.bruce.manifest` section after linking.

Alternatively, you can skip the external `manifest.json` and use the
`BRUCE_APP_MANIFEST(...)` macro once in your main file to embed the manifest
section at build time.

See `native_apps/examples/` for complete examples.

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
