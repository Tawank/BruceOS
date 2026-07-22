# BruceIDF Migration Plan

This plan migrates behavior from `BrucePIO/` into a clean ESP-IDF
implementation.  Nothing in `BrucePIO/` is changed or removed.  Each completed
slice has exactly one Core implementation and routes all front ends through it.

## Rules for every stage

1. Add or adjust a documented public Core API first.
2. Implement Core behavior with ESP-IDF only in Core code.
3. Route the built-in module, terminal command, and JS binding through it.
4. Add focused host/unit tests where possible and device smoke tests otherwise.
5. Do not copy a menu, CLI callback, or JS binding directly into Core.
6. Keep BrucePIO behavior as the migration reference; do not edit it.
7. Public capability APIs are named exactly `module__action()`.  Do not add
   `bruce_`-prefixed aliases or SDK forwarding implementations for a capability
   that already exists in Core.

## Stage 0 — Contract and build guardrails

Deliver the public SDK header layout and shared `BRUCE_*` error vocabulary.

- Keep private implementation headers in `src/core/` and public SDK headers
  in `src/core_sdk/`.  SDK callers must include the full `"core_sdk/..."`
  path (for example, `"core_sdk/result.h"`) rather than a bare filename.
- Define task IDs, task states, task snapshots, resource IDs, file IDs, and
  manifest data structures.
- Enable FreeRTOS runtime statistics for sampled CPU reporting.
- Establish naming: `module__action()` and field-specific Config APIs.  For
  example, the public Wi-Fi declaration is `wifi__scan()`, never
  `bruce_wifi__scan()`; do not add a duplicate `wifi_sdk.c` wrapper around
  `wifi_common.c`, which returns `BRUCE_*` results directly.
- Add compile checks preventing modules from including private Core headers.

Exit criteria: a built-in module compiles using only public SDK headers and the
current application build remains valid.

## Stage 1 — Runtime, Task, and memory foundation

Deliver the lifetime model before adding dynamic apps.

- Implement task creation, foreground stack, backgrounding, foregrounding,
  cooperative stop/pause/resume, force kill, task listing, and live-only wait.
- Implement task-local universal resource registry and reverse-order cleanup.
- Implement `memory__malloc()`/`memory__free()` accounting and automatic leak
  cleanup.
- Implement `runtime__sleep()` and `runtime__delay()`.
- Serialize display HAL calls and establish foreground-only physical input.

Exit criteria: a test module can allocate memory, background itself, be killed,
and leave no tracked memory/file resources behind.

## Stage 2 — app_runner and loaders

Deliver uniform launch behavior.

- Implement built-in registration and deterministic named resolution.
- Implement `app_runner__run()`, shell-style argument parsing, and task-ID
  result/error behavior.
- Implement `elf__run_path()` and `js__run_path()` path validation.
- Implement app_runner `STARTING` state and `--gui` task context.
- Add built-in `launcher` utility and `run_launcher_app()` fallback behavior.

Exit criteria: a uniquely named built-in app starts foreground/background with
correct arguments and return/task behavior.

## Stage 3 — Manifest, ELF, and JavaScript runtime

Deliver external app loading.

- Add `.bruce.manifest` parsing/validation, icon decoding, ABI warning, stack
  validation, and architecture validation.
- Add the SDK manifest generator/macro and public-symbol allowlist.
- Integrate Espressif ELF loader with task-owned loader allocations.
- Reject unresolved `malloc`/`free` imports; expose only public SDK symbols.
- Add optional leading JS manifest parsing and zero-permission defaults.
- Use `memory__malloc()` for the mQuickJS VM/context; preserve JS timers,
  `runtime.main()`, and optional `app_main(argv)`.
- Add `elf__inspect_path()` and `js__inspect_path()`.

Exit criteria: a minimal ELF and JS app load, show metadata, run in a task, and
cleanly unload/exit.

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

- `BrucePIO/src/core/wifi/`
- `BrucePIO/src/core/serial_commands/wifi_commands.cpp`
- `BrucePIO/src/modules/bjs_interpreter/wifi_js.cpp`
- `BrucePIO/src/core/menu_items/WifiMenu.cpp`

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

- Implement terminal as a thin parser: first token is app name, remainder is
  app_runner `arg`.
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

## Deferred work

- Ed25519 signing of the canonical manifest plus ELF loadable-content hash.
- Hardware/process isolation for hostile native apps.  v1 native apps are
  trusted extensions with a restricted ABI, not a hard sandbox.
- Per-app private persistent data namespaces beyond storage permission.
- Display compositor/layers.
- Async Core hardware APIs and task history.
