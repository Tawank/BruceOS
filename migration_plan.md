# BruceIDF Migration Plan

This plan migrates behavior from `BrucePIO_legacy/` into a clean ESP-IDF
implementation.  Nothing in `BrucePIO_legacy/` is changed or removed.  Each completed
slice has exactly one Core implementation and routes all front ends through it.

## Rules for every stage

1. Add or adjust a documented public Core API first.
2. Implement Core behavior with ESP-IDF only in Core code.
3. Route the built-in module, terminal command, and JS binding through it.
4. Add focused host/unit tests where possible and device smoke tests otherwise.
5. Do not copy a menu, CLI callback, or JS binding directly into Core.
6. Keep BrucePIO_legacy behavior as the migration reference; do not edit it.
7. Public capability APIs are named exactly `module__action()`.  Do not add
   `bruce_`-prefixed aliases or SDK forwarding implementations for a capability
   that already exists in Core.

## Stage 0 — Contract and build guardrails

Deliver the public SDK header layout and shared `BRUCE_*` error vocabulary.

- Keep private implementation headers in `src/core/` and public SDK headers
  in `src/core_sdk/`.  SDK callers must include the full `"core_sdk/..."`
  path (for example, `"core_sdk/result.h"`) rather than a bare filename.
- Define process IDs, process states, process snapshots, resource IDs, file IDs, and
  manifest data structures.
- Enable FreeRTOS runtime statistics for sampled CPU reporting.
- Establish naming: `module__action()` and field-specific Config APIs.  For
  example, the public Wi-Fi declaration is `wifi__scan()`, never
  `bruce_wifi__scan()`; do not add a duplicate `wifi_sdk.c` wrapper around
  `wifi_common.c`, which returns `BRUCE_*` results directly.
- Add SDK-only compile smoke targets in `src/CMakeLists.txt` that build key
  built-in modules against the public `core_sdk/` include namespace; enforcement
  that private Core headers are not included is currently by source review.

Exit criteria: a built-in module compiles using only public SDK headers and the
current application build remains valid.

## Stage 1 — Runtime, Process, and memory foundation

Deliver the lifetime model before adding dynamic apps.

- Implement process creation, foreground stack, backgrounding, foregrounding,
  cooperative stop/pause/resume, force kill, process listing, and live-only wait.
- Implement process-local universal resource registry and reverse-order cleanup.
- Implement `memory__malloc()`/`memory__free()` accounting and automatic leak
  cleanup.
- Implement `runtime__sleep()` and `runtime__delay()`.
- Serialize display HAL calls and establish foreground-only physical input.

Exit criteria: a test module can allocate memory, background itself, be killed,
and leave no tracked memory/file resources behind.

## Stage 2 — app_runner and loaders

Deliver uniform launch behavior.

- Implement built-in registration and deterministic named resolution.
- Implement `app_runner__run()`, shell-style argument parsing, and process-ID
  result/error behavior.
- Implement placeholder `.elf`/`.js` path validation ahead of Stage 3's
  loader registry (`app_runner__register_loader()`, `app_runner__run_path()`).
- Implement app_runner `STARTING` state and `--gui` process context.
- Add built-in `launcher` utility and `run_launcher_app()` fallback behavior.

Exit criteria: a uniquely named built-in app starts foreground/background with
correct arguments and return/process behavior.

## Stage 3 — Loader registry, ELF, and JavaScript loader modules

Deliver external app loading as pluggable loader modules instead of Core
internals, so ELF, JavaScript, and any future format (Python, etc.) share one
registration contract and no format gets special Core access.

- Add the app_runner loader registry: `app_runner__register_loader()`,
  `app_runner__run_path()`, and `app_runner__spawn_loader_process()` (the
  public primitive a loader uses to turn a decoded image into a
  permission-checked, resource-tracked process without any private Core header).
  Refactor Stage 2's placeholder `/bin/<name>.elf` -> `/bin/<name>.js`
  resolution to iterate this registry.
- Add universal manifest inspection in `core/manifest/` /
  `core_sdk/manifest.h`: `manifest__inspect_path()` auto-detects file format
  (ELF magic, JS comment block, etc.) and returns a heap-allocated raw
  manifest JSON string (caller frees); `manifest__inspect_elf()` validates
  the ELF32 header (magic, `e_machine`) and returns a heap-allocated parsed
  `bruce_app_inspection_t` (caller frees with `memory__free()`);
  `manifest__inspect_javascript()` extracts the JS comment block;
  `manifest__parse()` parses JSON into `bruce_manifest_t`.  Per-loader
  inspection functions (`elf__inspect_path()`, `js__inspect_path()`) are not
  needed — `core/manifest` owns format-aware extraction so it is never
  duplicated.
- Add the built-in ELF loader module (`src/modules/loaders/elf/`): registers
  `.elf` at priority 10, calls `manifest__inspect_elf()` for mandatory
  manifest validation, integrates the Espressif ELF loader with
  `app_runner__spawn_loader_process()`-owned allocations, exposes a public
  symbol allowlist, and rejects imported `malloc`/`free`.  Its
  process entry is `elf_loader__app_main(void *context)`.
- Expose the built-in ELF loader as a command named `elf` so that
  `elf ./app.elf <args>...` loads the named ELF directly.  This lets an
  ELF app itself act as a loader (e.g. `elf ./elf_loader.elf ./game.elf`),
  with the loaded app calling `app_runner__run_path()` to load another ELF.
- Provide SDK ELF build tooling (`elf_apps/include/bruce_sdk.h`,
  `elf_apps/tools/build_elf_apps.py`) and template apps in `elf_apps/examples/`.
- Add the built-in JavaScript loader module (`src/modules/loaders/js/`):
  registers `.js` at priority 20, parses an optional leading manifest with
  zero-permission defaults, and uses `memory__malloc()` for the mQuickJS
  VM/context; preserves JS timers, `runtime.main()`, and optional
  `app_main(argv)`.  Its process entry is `js__app_main(void *context)`.

Exit criteria: a minimal ELF and JS app load, show metadata, run in a process,
and cleanly unload/exit; a third, independent loader module can register a
new extension using only `core_sdk/` headers, with zero changes to Core;
`manifest__inspect_path()` inspects ELF, JS, and any registered extension
uniformly without involving the loader module; the built-in `elf` command can
load an ELF by path and an ELF loader app can load another ELF.

## Stage 4 — Permissions, Config, Storage, and dialogs

Deliver the policy boundary used by every later capability.

- Implement `/permissions.json` using filename-with-extension keys.
- Implement manifest preflight requests, runtime first-use prompts, persistent
  allow/deny decisions, and built-in implicit grants.
- Implement renderer-neutral `dialog__*`, GUI/terminal selection by `--gui`,
  picker path filtering, and text-viewer handles.
- Implement field-specific Config getters/setters, protected fields, immediate
  validation/persistence, and `config` checks.
- Implement opaque Storage handles, protected paths, `storage` checks, and
  automatic close at teardown.

Exit criteria: an external app can be denied/granted at runtime, cannot read
protected config files/fields, and a built-in can manage configuration.

## Stage 5 — Wi-Fi vertical slice (first feature migration)

Migrate the first full feature from these legacy sources:

- `BrucePIO_legacy/src/core/wifi/`
- `BrucePIO_legacy/src/core/serial_commands/wifi_commands.cpp`
- `BrucePIO_legacy/src/modules/bjs_interpreter/wifi_js.cpp`
- `BrucePIO_legacy/src/core/menu_items/WifiMenu.cpp`

Work:

- Complete permission-checked synchronous `wifi__*` and `http__*` APIs.
- Keep JS names such as `wifi.scan()` and `httpFetch()`; replace direct Arduino
  calls with Core calls.
- Make the terminal `wifi` command invoke the Wi-Fi app through app_runner.
- Make the launcher Wi-Fi module compose UI and invoke the same APIs.
- Verify `wifi` and `http` are independently checked.

Exit criteria: GUI, CLI, JS, and ELF use one Core Wi-Fi implementation and
produce equivalent scan/connect/disconnect behavior.

## Stage 6 — Input, display, and interaction migration

Migrate common UI primitives before feature menus.

- Port input queue, normalized injection, and legacy JS keyboard helpers.
- Port display HAL methods and route existing JS `display.*` bindings to them.
- Route JS `dialog.*` common methods to Core dialog APIs.
- Preserve `dialog.pickFile()` and text-viewer handles; omit `viewFile()`.

Exit criteria: a JS or ELF controller adapter can inject input; a foreground
app receives it; GUI and terminal dialog paths both work.

## Stage 7 — Terminal and launcher migration

- Implement `serial_commands` as a thin parser: first token is app name,
  remainder is app_runner `arg`; the GUI terminal uses the same parser through
  a captured Core stdio session.
- Route JS `serial.cmd()` to the same parser.
- Port `bruce_launcher` menu composition from legacy `menu_items`; each feature
  becomes a module command instead of Core menu code.
- Add `/apps/` discovery entirely inside the launcher/file-manager modules.

Exit criteria: the same command can be run from terminal, launcher, JS, and
another ELF app without feature-specific dispatch code in Core.

## Stage 8 — Remaining vertical feature slices

Migrate one capability at a time, using the same four-front-end rule:

1. configuration screen and terminal configuration commands;
2. storage/file manager;
3. Bluetooth;
4. RF/LoRa/NRF24;
5. IR;
6. RFID;
7. GPS;
8. GPIO/I²C and controller adapters;
9. HID/BadUSB;
10. microphone and remaining audio/display-adjacent features.

Each slice first adds tested Core API behavior, then migrates its JS bindings,
terminal path, and launcher module.

The compositor-adjacent notification and status-icon slice is implemented:
notifications are transient Core-composited overlays, while status icons remain
an application-rendered global registry exposed through C/ELF, JavaScript, the
launcher, and the built-in `notification` terminal command.

## Deferred work

- Ed25519 signing of the canonical manifest plus ELF loadable-content hash.
- Hardware/process isolation for hostile native apps.  v1 native apps are
  trusted extensions with a restricted ABI, not a hard sandbox.
- Per-app private persistent data namespaces beyond storage permission.
- Rich display layers beyond the implemented small viewport compositor.
- Async Core hardware APIs and process history.
