# Agent Task Briefs

These briefs are intentionally scoped so agents can work independently once
their listed dependencies are complete.  Every implementation must follow
[`migration_BruceIDF.md`](migration_BruceIDF.md) and use `BrucePIO/` only as a
read-only behavior reference.

## A1 — Public SDK and error model

Dependencies: none.

Create the public SDK header layout in `src/core_sdk/` and `BRUCE_*` result-code definitions.
Define opaque task/file/viewer IDs and public structs needed by Task, AppRunner,
Storage, dialog, input, and manifest inspection.  Keep implementation headers
in `src/core/`; ensure built-in modules compile only against `src/core_sdk/`.
SDK imports use their full `"core_sdk/..."` path, never a bare filename, to
avoid ambiguous headers such as `result.h`.
Public capability functions must use `module__action()` names exactly: declare
the existing Core symbol in the SDK rather than adding a `bruce_` alias or a
duplicate forwarding implementation (for example, `wifi_common.c` owns
`wifi__*`, so do not add `wifi_sdk.c`).  A public function does not have to
return `bruce_result_t`; use `bool`/`int`/a pointer when that is easier for
callers, and document the exact meaning in the `core_sdk/` header.  Whatever
the return type, the Core-private header (`src/core/<module>/*_common.h`)
must never redeclare the public struct/signatures that already live in
`src/core_sdk/<module>.h` — the `.c` file includes the `core_sdk/` header
directly for those, keeping the private header for genuinely private
declarations only, so the two can never drift into conflicting redeclarations.

Acceptance: a compile-time test proves private headers are not required by a
built-in module;
Except functions which return `bool`/`int`/pointer, all public functions return `bruce_result_t`.

## A2 — Task/runtime/resource registry

Dependencies: A1.

Implement Core task records, foreground stack, task-local resource registry,
memory tracking, cooperative state control, force kill, live task snapshots,
and immediate cleanup.  Include CPU sampling and interruptible sleep.

Acceptance: a test app backgrounds/foregrounds, allocates tracked memory,
opens a tracked resource, then exits or is killed with all resources released.

## A3 — AppRunner and built-in registration

Dependencies: A1, A2.

Implement built-in registration, duplicate rejection, named resolution, the
exact `app_runner__run(const char *, const char *, bool)` API, argument parsing,
and `STARTING` task context.  Add the built-in `launcher` utility and fallback
to `bruce_launcher`.

Acceptance: built-in command precedence, ELF/JS fallback order, argument
quoting, task IDs, and background startup all have tests.

## A4 — Permissions and dialogs

Dependencies: A1, A2, A3.

Implement `/permissions.json`, coarse permission checks, manifest preflight,
dynamic first-use requests, persistent denials, built-in implicit grants, and
the renderer-neutral dialog layer.  Implement GUI/terminal dispatch from the
task’s preserved `--gui` context.

Acceptance: tests cover allow, deny, no reprompt after denial, same-basename
sharing, dynamic request, and GUI/CLI choice rendering.

## A5 — Config and Storage APIs

Dependencies: A1, A2, A4.

Replace whole-config external access with field-specific Config APIs.  Add
`config` enforcement, permanent external denial of the protected Wi-Fi/WebUI
fields, validation, and atomic writes.  Add opaque Storage handles, `storage`
enforcement, protected paths, automatic close, directory listing, and picker
support.

Acceptance: external tests cannot access protected config fields/files; a
storage-granted task can use other paths and leaks no file handles.

## A6 — Loader registry and ELF loader module

Dependencies: A1, A2, A3, A4.

ELF and JS are not Core internals: they are modules that register themselves
with app_runner's loader registry, the same way any third party could
register a `.py` or other loader without touching Core.  First add the
registry itself: `app_runner__register_loader(extension, priority, run_fn)`
in `core_sdk/loader.h`/`core/app_runner/`, the loader-agnostic
`app_runner__run_path()`, and `app_runner__spawn_loader_task()` (the one
extra public primitive a loader needs to turn a decoded image into a
permission-checked, resource-tracked Core task without any private header).
Refactor A3's hardcoded `/bin/<name>.elf` -> `/bin/<name>.js` resolution
inside `app_runner__run()` to iterate this registry instead, with no change
to `app_runner__run()`'s observable behavior.

Also add universal manifest inspection in `core/manifest/` /
`core_sdk/manifest.h`: `manifest__inspect_path()` auto-detects file format
(ELF magic, JS comment block, JPG, PNG etc.) and returns the raw manifest JSON bytes;
`manifest__inspect_elf()` validates the ELF32 header (magic, `e_machine` vs.
this build's target) and returns a parsed `bruce_app_inspection_t`.  The
loader registry's `app_runner__register_loader()` no longer takes an
`inspect_fn` — any program (launcher, file manager, terminal) inspects
files with `manifest__inspect_path()` directly; ELF-specific programs use
`manifest__inspect_elf()`.

Then, as the registry's first consumer, add the built-in ELF loader module
under `src/modules/loaders/elf/` (not `src/core/elf/`): integrate the
Espressif ELF loader, call `manifest__inspect_elf()` for mandatory
`.bruce.manifest` parsing, ABI warning, target check, and 32×32 icon
decoding, use `app_runner__spawn_loader_task()`-owned allocations, expose a
public symbol resolver, and reject imported `malloc`/`free`.  Register it
for `.elf` at priority 10.  The loader module's task entry (the function
passed as `entry` to `app_runner__spawn_loader_task()`) is
`elf_loader__app_main(void *context)`.

Also expose the loader as a built-in command named `elf`, so that
`elf ./app.elf <args>...` loads the named ELF file and passes the
remaining arguments to it.  This enables loader chains: the built-in
`elf` command can load an ELF loader app such as `./elf_loader.elf`, and
`elf_loader.elf` (with the `execute` permission) can call
`app_runner__run_path()` to load `./game.elf`.

Provide SDK manifest build tooling: `elf_apps/include/bruce_sdk.h` with the
`BRUCE_APP_MANIFEST()` macro, `elf_apps/tools/build_elf_apps.py` to inject the
non-allocatable `.bruce.manifest` section, and template apps in
`elf_apps/examples/` (including `elf_loader` and `game`).

The loader module includes only `core_sdk/...` headers, the same as any
other built-in — it gets no `modules/selftest`-style private-header
exemption.

Acceptance: a third, throwaway test loader module can register a new
extension using only the public registry API with zero Core changes;
`manifest__inspect_elf()` inspects an ELF file without involving the loader
module; a minimal ELF loads from `/bin`, exposes only approved imports,
prompts for requested permissions, and frees image memory at exit; the
built-in `elf` command loads an ELF by absolute path; the SDK build tooling
can produce a loader ELF that loads another ELF.

## A7 — JavaScript loader module and bridge conversion

Dependencies: A2, A3, A4, A5, A6.

As the registry's second consumer, add the built-in JavaScript loader module
under `src/modules/loaders/js/` (not `src/core/js/`): register `.js` at
priority 20 with `app_runner__register_loader()` (no `inspect_fn`), call
`manifest__inspect_path()` to extract raw manifest JSON from the JS comment
block, then `manifest__parse()` for optional leading manifest parsing with
zero-permission fallback,
`app_runner__spawn_loader_task()`-owned mQuickJS allocation (VM/context
memory via `memory__malloc()`), optional `app_main(argv)`, and
`js__app_main(void *context)` as the loader's task entry.  Preserve the
existing JS API names and rewrite each binding to call Core APIs instead of
Arduino/legacy globals.  Keep timers and `runtime.main()` inside the
JavaScript runtime.

Acceptance: old-style `wifi.scan()`, `display.*`, `dialog.*`, and
`serial.cmd()` bindings use Core APIs; no binding directly calls ESP-IDF or
Arduino facilities; the JS loader module includes only `core_sdk/...`
headers.

## A8 — Wi-Fi vertical slice

Dependencies: A1–A7.

Finish permission-checked Core Wi-Fi and HTTP APIs, then migrate the Wi-Fi
module, terminal command, and JS binding from the listed BrucePIO sources.
Use `wifi` for radio control and `http` independently for HTTP.

Acceptance: equivalent Wi-Fi scan/connect/disconnect behavior through GUI,
terminal, JS, and ELF; test both permission denials.

## A9 — Input/display/dialog vertical slice

Dependencies: A1, A2, A4, A7.

Implement normalized input, foreground queue, injection, serialized display,
generic dialog methods, picker filtering, and tracked text-viewer handles.
Migrate legacy JS input/display/dialog bindings.

Acceptance: a GPIO/I²C/Bluetooth adapter can inject an event to a foreground
app; GUI and serial dialogs work; viewer/file handles are cleaned up.

## A10 — Terminal and launcher modules

Dependencies: A3, A4, A5, A6, A7, A8, A9.

Implement terminal as AppRunner parser.  Port `bruce_launcher` as menu-only
composition; add `/apps/` discovery using inspection APIs.  Convert legacy
`menu_items` one-by-one into modules rather than Core menu code.

Acceptance: terminal and launcher execute the same named/path applications;
Core contains no feature menu or serial-feature dispatcher.

## A11 — Remaining capability slices

Dependencies: A1–A10.

For each remaining BrucePIO capability (Bluetooth, RF, IR, RFID, GPS,
GPIO/I²C, HID, microphone, configuration, and file manager), implement Core
first, then migrate its JS, terminal, and launcher paths.  Work one feature per
change set and do not mix unrelated feature migrations.

Acceptance: no migrated capability retains duplicate direct hardware code in
JS bindings, serial commands, or launcher menus.
