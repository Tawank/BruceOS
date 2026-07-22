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

## A6 — ELF loader and manifest tooling

Dependencies: A1, A2, A3, A4.

Integrate Espressif ELF loader.  Implement mandatory `.bruce.manifest` parsing,
validation, ABI warning, target check, 32×32 icon decoding, task-owned loader
allocations, public symbol resolver, and rejection of imported `malloc/free`.
Provide SDK manifest build tooling and `elf__inspect_path()`.

Acceptance: a minimal ELF loads from `/bin`, exposes only approved imports,
prompts for requested permissions, and frees image memory at exit.

## A7 — JavaScript runner and bridge conversion

Dependencies: A2, A3, A4, A5.

Implement `js__run_path()`, optional leading manifest parsing, zero-permission
fallback, task-owned mQuickJS allocation, optional `app_main(argv)`, and
`js__inspect_path()`.  Preserve the existing JS API names and rewrite each
binding to call Core APIs instead of Arduino/legacy globals.  Keep timers and
`runtime.main()` inside JavaScript runtime.

Acceptance: old-style `wifi.scan()`, `display.*`, `dialog.*`, and
`serial.cmd()` bindings use Core APIs; no binding directly calls ESP-IDF or
Arduino facilities.

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
