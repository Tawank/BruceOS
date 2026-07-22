# BruceIDF Architecture Contract

## Purpose

BruceIDF is a new ESP-IDF implementation of Bruce OS.  `BrucePIO/` is a
reference repository to migrate feature-by-feature; it is not code to modify,
delete, or refactor in place.

The Core is deliberately small.  It owns runtime state, policy, hardware, and
safe application interfaces.  Everything a person uses is an application or a
module above that Core.

The design goal is one implementation per capability:

```
ELF application ─┐
JavaScript app ──┼─> public Core API ─> ESP-IDF / hardware
terminal command ┤
launcher module ─┘
```

No JavaScript binding, serial command, launcher menu, or feature module may
implement a second copy of Wi-Fi, storage, configuration, display, or other
hardware behavior.

## Boundaries

### Core owns

- bootstrap and system configuration;
- task, runtime, and app_runner, including its pluggable loader registry;
- permission decisions and resource ownership;
- memory, file, dialog, input, display, network, radio, and other HAL APIs;
- all ESP-IDF calls and hardware handles.

Core has no launcher menu, application menu, theme policy, or feature-specific
screen.  Renderer-neutral primitives such as `dialog__choice()` are Core APIs;
the active task context selects a GUI or terminal renderer.  Core also does
not own any particular file-format loader: it owns the registry that lets a
module claim an extension, not the ELF loader or JavaScript runner
themselves (see "Loader modules" below).

### Apps and modules own

- menus, launchers, terminal UX, file-manager UX, settings screens, and
  feature workflows;
- translating user intent into public Core API calls;
- application-specific presentation and state;
- loader modules: the code that turns a file of one specific extension
  (`.elf`, `.js`, or any other third-party format such as `.py`) into a
  running task.  A loader module registers itself with app_runner's loader
  registry; being "the ELF loader" or "the JS runner" gives it no Core
  access that a new third-party loader module would not also have.

Built-in modules are compiled into firmware.  ELF and JavaScript apps are
external applications.  Built-ins use the same public SDK API and lifecycle as
external apps, but their permission checks always pass.  Neither kind of app
uses private Core headers or ESP-IDF directly.

The one deliberate exception is `modules/selftest`, a built-in diagnostic app
whose entire purpose is validating Core's private implementation (task
registry, memory tracking, resource cleanup, and so on as Core grows).  It is
allowed to include `src/core/` private headers and call FreeRTOS/ESP-IDF
directly, the same way Core source itself does.  It still has no special
launch path: it is registered and run through app_runner like any other
built-in (`app_runner__run("selftest", ...)`, from the launcher, or from the
terminal), and it is never a dependency of another app or module.  No other
built-in gets this exemption.

## Bootstrap and launcher selection

`main.c` initializes Core services (including display and configuration), then
runs the built-in utility command `launcher`.

`launcher` is a small module under `modules/utils/`.  Its
`run_launcher_app()` helper reads `launcherApp` from `bruce.json` using the
public Config API and starts that command with app_runner.  The default is
`bruce_launcher`; an empty or unstartable configured value falls back to
`bruce_launcher`.

`bruce_launcher` is an application, not Core.  It composes menus and may scan
`/apps/`.  It appends `--gui` to every app it launches.

## Applications and app_runner

### Entry points

- Every ELF exports `int app_main(int argc, char **argv)`.
- Every built-in registers a uniquely named C function with that same
  signature.  It cannot use the literal global `app_main`, because firmware
  contains multiple modules.
- A JavaScript file is evaluated first.  If it defines `app_main(argv)`, the
  JavaScript event loop invokes it; it is optional.

### Named execution

The public named-run API is:

```c
int app_runner__run(const char *app_name, const char *arg, bool in_background);
```

It returns a positive Core task ID on success and a negative `BRUCE_ERR_*`
error code on failure.  `arg` is a shell-style argument string (quotes and
backslash escaping); empty or `NULL` creates `argc == 0`.

Resolution is deterministic and never scans directories:

1. registered built-in module;
2. every registered loader, tried in ascending priority order, matching
   `/bin/<app_name><extension>`;
3. `BRUCE_ERR_NOT_FOUND`.

Core ships a built-in ELF loader module registered at priority 10 and a
built-in JavaScript loader module registered at priority 20, so ELF still
wins if both `/bin/<app_name>.elf` and `/bin/<app_name>.js` exist.  A
third-party loader module (a `.py` loader, for example) registers its own
extension and priority the same way and is resolved by the same loop, with no
Core change required.  Duplicate built-in command names and duplicate loader
extensions are both startup registration errors.

Any caller — including `app_runner__run()` itself — that needs to start an
arbitrary path rather than a `/bin/<name>` command uses the loader-agnostic:

```c
int app_runner__run_path(const char *path, const char *arg, bool in_background);
```

which accepts only normalized absolute paths with no `.` or `..` components,
looks up the loader registered for the path's extension, and dispatches to
it.  An app with `execute` may start an ELF, JS, or any other
loader-registered file type from any mounted path this way.  Core does not
discover `/apps/`, `/sdcard/`, or any other folder; launcher and
file-manager apps decide what to enumerate.

Launching another app does not inherit or intersect permissions.  Each target
is its own user-controlled task.

### Loader modules

A loader module owns everything specific to one file format: recognizing its
manifest, decoding/relocating/interpreting its content, and running it.  It
registers itself with:

```c
bruce_result_t app_runner__register_loader(const char *extension, int priority,
                                            bruce_loader_run_fn run_fn,
                                            bruce_loader_inspect_fn inspect_fn);
```

`extension` includes the leading dot (for example `.elf`); `priority` breaks
ties when more than one candidate file matches an app name, lower first.
`run_fn` matches `app_runner__run_path()`'s signature; `inspect_fn` matches
`app_runner__inspect_path()`'s and never launches anything.  Registration
happens once at boot, alongside built-in command registration, before the
first named-run or path-run call.  A duplicate extension registration is a
startup error, the same as a duplicate built-in command name.

Turning a decoded image into a live task needs more than an ordinary built-in
gets from `app_runner__register()`, so loaders get one extra public
primitive:

```c
int app_runner__spawn_loader_task(const char *permission_key, bool gui_requested,
                                   bool in_background, uint32_t stack_size,
                                   bruce_loader_task_entry_fn entry, void *ctx);
```

This creates a real Core task that calls `entry(ctx)` on its own stack, wired
into the same foreground stack, resource registry, and permission checks
(`permission_key`, e.g. `"game.elf"`) as any other task.  It is the only new
capability a loader author needs beyond the rest of the public SDK
(`memory__malloc()`/`memory__free()` for image/VM storage, which are freed
automatically at task exit or kill with no custom cleanup callback needed).
A loader module still includes only `core_sdk/...` headers — it gets no
private-header exemption, unlike `modules/selftest`.

Manifest parsing itself is shared, not duplicated per loader: every loader
extracts its own raw manifest bytes (an ELF section, a leading JS comment
block, or whatever a new format uses) and hands them to the one canonical
parser/validator in `core_sdk/manifest.h`, so every loader enforces the same
required fields, ABI check, stack bounds, icon decoding, and permission-name
validation without reimplementing JSON handling.

Built-in ELF and JavaScript loader modules live under `src/modules/loaders/`,
the same as any other module; they have no special standing over a
third-party loader someone else registers the same way.

### Task lifecycle

Every run creates a Core-managed task.  A normal named run starts foreground;
`in_background` supports autostart and services.  The Core keeps a foreground
stack: foregrounding a new task pushes the prior task; exit or
`runtime__to_background()` restores the most recent foreground task, falling
back to `bruce_launcher`.

Backgrounding changes state and physical-input ownership only.  It does not
suspend the task or revoke display/serial use.  Display drawing is shared and
serialized; v1 has immediate-mode, last-completed-update-wins semantics.

Tasks are identified by opaque nonzero `uint32_t` IDs, never FreeRTOS handles.
`task__list()` returns read-only snapshots of live tasks including state,
stack high-water mark, sampled CPU percentage, and memory/resource statistics.
Completed tasks are cleaned up immediately; v1 has no history.
`task__wait()` works only while a task still exists.

`stop`, `pause`, and `resume` are cooperative.  `task__kill()` is an explicit
force-delete escape hatch.  Controlling another task requires `task`; every
app can control only itself without that permission.

`runtime__sleep(ms)` is interrupted when the task is foregrounded.
`runtime__delay(ms)` waits the requested duration.  Both are Core APIs; they
hide FreeRTOS from apps.

## ELF contract

Every ELF contains a non-loadable `.bruce.manifest` section.  The built-in ELF
loader module reads and validates it before relocation or entry.  The SDK
macro/tool `BRUCE_APP_MANIFEST(...)` emits this section; authors do not
hand-write ELF section attributes.

The manifest is canonical UTF-8 JSON with fixed field order and no
insignificant whitespace.  Permission array order is author-controlled, but
duplicates are invalid.  Required fields are `appName`, `appIcon`,
`coreAbiVersion`, and `stackSize`; `permissions` is optional and defaults to an
empty array.  A complete example is:

```json
{
  "appName": "Example app",
  "appIcon": "<Base64 128-byte bitmap>",
  "coreAbiVersion": 1,
  "permissions": ["wifi", "http"],
  "stackSize": 8192
}
```

- `appName` is the launcher/task-manager display name.  The filename is the
  command identity; there is no `appId`.
- `appIcon` decodes to exactly 128 bytes: 32×32 monochrome, row-major,
  most-significant-bit leftmost.  A 1 uses launcher foreground colour; 0 uses
  launcher background colour.
- `stackSize` is bytes, constrained to 4–16 KiB.  SDK tooling uses 8 KiB by
  default.
- An omitted or empty `permissions` array gives a safe zero-permission ELF
  fallback.
- A Core ABI mismatch shows a warning and requires an explicit user choice to
  run anyway.
- The ELF header, not the manifest, is authoritative for architecture; a
  mismatching chip architecture is rejected.

Ed25519 signing is deferred.  Canonical manifest bytes and the ELF
loadable-content hash are the future signing inputs.

The ELF loader module's `elf_find_sym()` resolves only the documented public
SDK symbol allowlist it maintains.  It never resolves ESP-IDF, private Core,
`malloc`, or `free` symbols.  The SDK uses `memory__malloc()` and
`memory__free()`; imports named `malloc` or `free` are rejected.  A trusted
native app can still embed a custom allocator, which is unsupported rather
than a hard sandbox violation.

## JavaScript contract

The built-in JavaScript loader module evaluates the script and owns its
mQuickJS event loop.  JavaScript timers and `runtime.main()` remain
JavaScript-runtime features; they are not moved into Core.  The loader
allocates VM/context memory with `memory__malloc()`.

A JS file may start with the same canonical manifest in a comment block.  It
is optional.  An unmanifested script starts with zero grants, uses its filename
as display name, a generic icon, and the mQuickJS default task stack.

Preserve the existing JS surface as much as possible: `wifi.scan()`,
`dialog.choice()`, `display.*`, `runtime.*`, `serial.cmd()`, and similar APIs
remain.  Their bindings are replaced internally to call the public Core APIs;
they are not renamed to a new `bruce.*` namespace.

`runtime.main()` is retained for now even though optional `app_main(argv)` is
the normal JS lifecycle entry.  `serial.cmd(command)` delegates to the same
terminal/app_runner parser and requires `execute`.

## Permissions

Permissions are coarse-grained.  The current vocabulary is:

```
http, wifi, bt, gps, rf, input, gpio, ir, rfid, microphone,
hid, execute, task, storage, config, serial
```

`gpio` includes I²C.  `rf` includes Sub-GHz, LoRa, and NRF24.  `audio` is not
permission-gated.  Unknown permission names are invalid.

Permissions are stored in Core-owned `/permissions.json`, keyed only by the
filename including extension and without its path:

```json
{
  "game.elf": { "wifi": 0, "bt": 1 },
  "weather.js": { "http": 1 }
}
```

Files with the same basename deliberately share this decision, even from
different folders.  Use different filenames when separate decisions are
needed.  The file remains simple: there is no source/provenance metadata and
Core never prunes saved grants or denials.

Manifest permissions are a pre-launch request list.  Missing decisions are
shown together, unchecked by default, and saved after the user chooses.  A
permission not declared in the manifest may be requested dynamically on its
first protected API call.  A saved `0` denies immediately without reprompting;
a permissions-management UI is the way to change it.

Built-in module tasks are implicitly granted every permission.  External
tasks are checked inside each protected Core API.

`http__request()` needs `http`; it does not imply `wifi`.  Wi-Fi state control
and credentials need `wifi`.  `input__inject()` needs `input`; task control of
another task needs `task`; starting a path needs `execute`.

## Dialog and task interaction

`dialog__*` is renderer-agnostic.  `dialog__choice()` displays a GUI choice
for tasks launched with `--gui`, or a terminal choice such as:

```
1. yes
2. no
pick: _
```

app_runner records the initial `--gui` launch context in task-local storage
before launch-time permission checks, but leaves the argument in the app’s
`argv`.  A background serial-monitor-style task decides its own behavior;
there is no separate dynamic task interaction-mode API.

Retain the JS `dialog.message`, `info`, `success`, `warning`, `error`, and
`choice` APIs as wrappers.  Keep `dialog.pickFile()` as a Core renderer-neutral
API: it requires `storage` and hides `/bruce.json` and `/permissions.json`.
Do not migrate `dialog.viewFile()`.  `dialog__create_text_viewer()` returns an
opaque viewer ID with draw, scroll, set-text, and close operations; it is a
tracked resource.

## Memory and resource ownership

ELF apps never receive libc `malloc` or `free`.  They use
`memory__malloc()`/`memory__free()`.  Core allocates ELF images, BSS, loader
bookkeeping, and JS VM/context memory through this same task-owned allocator.

Every task has one universal resource registry in thread-local storage.  Core
services register opaque handles and Core-owned cleanup callbacks for memory,
files, sockets, viewers, radios, and similar resources.  Apps cannot register
arbitrary callbacks.  At normal exit or kill, Core reads the registry before
the task disappears and cleans resources in reverse acquisition order.

## Input, display, storage, and Config

Physical buttons, touch, keyboard, and encoder input go only to the foreground
task.  `input__read(input_event_t *, timeout_ms)` is a blocking foreground
queue read.  `input__inject()` accepts a normalized event with type, action,
code, value, timestamp, and source task ID, allowing Bluetooth, GPIO, and I²C
adapters to feed the same pipeline.

Display HAL calls are serialized by Core; v1 intentionally has no compositor
or layers.

`storage` grants access to Core `storage__*` APIs.  Public file handles are
opaque IDs and are closed automatically at task teardown.  The only v1
protected paths are `/bruce.json`, `/permissions.json`, and their atomic-write
temporary files; all other mounted paths are usable by a storage-granted app.

`config` grants field-specific APIs such as `config__get_bright()` and
`config__set_soundEnabled(true)`.  Setters validate and atomically persist
immediately.  The following fields are permanently protected from ELF and JS,
even with `config`: `wifiApSsid`, `webUIPassword`, `wifiCredentials`,
`wifiMAC`, and `webUIUser`.  Built-ins may use those APIs.

## Public SDK and migration rules

Maintain two header layers:

- `src/core/` contains private Core implementation headers and ESP-IDF
  details; only Core source may include them.
- `src/core_sdk/` contains the public Core SDK headers used by every built-in
  module and external ELF app.  SDK callers include them with their full,
  unambiguous namespace, for example `"core_sdk/app_runner.h"`.

`core_sdk/loader.h` declares the loader registry
(`app_runner__register_loader()`, `app_runner__run_path()`,
`app_runner__inspect_path()`, `app_runner__spawn_loader_task()`) that any
loader module uses.  Built-in ELF and JavaScript loader modules live under
`src/modules/loaders/elf/` and `src/modules/loaders/js/`, like any other
built-in module, not under `src/core/`.

The component exports the `src/` include root only so those `core_sdk/...`
includes resolve.  Built-in modules include only `core_sdk/...` headers;
their privilege is policy, not access to private C declarations.  The single
exception is the `modules/selftest` diagnostic built-in described under
"Apps and modules own", which may include `src/core/` headers directly; it is
excluded from the compile-time check (in `src/CMakeLists.txt`) that verifies
every other built-in only pulls in `core_sdk/...` headers.  New public
fallible APIs use shared `BRUCE_*` result codes such as `BRUCE_OK`,
`BRUCE_ERR_PERMISSION`, `BRUCE_ERR_NOT_FOUND`, and `BRUCE_ERR_BUSY` by
default, but `bruce_result_t` is not mandatory for every public function.
Use whatever return type is easiest for a caller when the operation cannot
fail or when a simpler type documents the failure case just as clearly, for
example `bool wifi__is_connected(void)`, `char *wifi__get_ssid(void)`
(`NULL` when unavailable), or `int wifi__scan(...)` (network count, or a
negative `BRUCE_*` value on failure).  Document the exact success/failure
values in the header comment above the declaration whenever the type is not
`bruce_result_t`.

### Private headers must not duplicate the public SDK declaration

A Core-private header (`src/core/<module>/<module>_common.h`) must never
redeclare the struct/function signatures that its matching `src/core_sdk/`
header already declares, even if the two currently look identical.  The
implementation `.c` file should `#include "core_sdk/<module>.h"` directly to
pick up the one authoritative declaration of the public contract it
implements, in addition to its own private header for genuinely
Core-internal-only declarations.  Two independent copies of the same public
signature drift the moment either side changes a parameter or return type
(this happened once already with `wifi__scan`/`wifi__is_connected`/
`wifi__get_ssid`/`wifi__get_ip`/`wifi__get_mac` and had to be fixed), and a
mismatched redeclaration is a hard compile error, not a warning.  If a
private header ends up with nothing left to declare, that is expected and
correct — leave it as a documented placeholder rather than re-adding the
public API to give it content.

### API naming and ownership

Public capability functions use exactly `module__action()` names.  For
example, Wi-Fi functions are `wifi__scan()`, `wifi__connect()`, and
`wifi__disconnect()`.  Do not add a `bruce_` function prefix or create a
second SDK wrapper such as `bruce_wifi__scan()`; the public declaration must
refer to the one Core implementation.  `BRUCE_*` is reserved for result codes,
constants, and public type prefixes where appropriate.

When an existing capability is exposed through `src/core_sdk/`, declare its
existing `module__action()` API there.  Do not add a forwarding `.c` file
unless the Core capability itself is genuinely missing.  In particular,
`wifi_common.c` remains the single Wi-Fi implementation; most of its
functions return the public `BRUCE_*` results directly, and the handful that
return `bool`/`int`/`char *` instead are documented in `core_sdk/wifi.h`.
A8 adds permission checks there.


Migrate in vertical slices.  For each capability, first create the Core API,
then route its JavaScript binding, terminal command, and launcher module to
that API.  Do not delete or change BrucePIO; use it only as the behavioral
reference.  Wi-Fi is the first complete vertical slice.
