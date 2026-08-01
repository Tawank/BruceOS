# BruceIDF Architecture Contract

## Purpose

BruceIDF is a new ESP-IDF implementation of Bruce OS.  `BrucePIO_legacy/` is a
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
- process, runtime, and app_runner, including its pluggable loader registry;
- permission decisions and resource ownership;
- memory, file, dialog, input, display, network, SSH, radio, and other HAL APIs;
- all ESP-IDF calls and hardware handles.

Core has no launcher menu, application menu, theme policy, or feature-specific
screen.  Renderer-neutral primitives such as `dialog__choice()` are Core APIs;
the active process context selects a GUI or terminal renderer.  Core also does
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
  running process.  A loader module registers itself with app_runner's loader
  registry; being "the ELF loader" or "the JS runner" gives it no Core
  access that a new third-party loader module would not also have.

Built-in modules are compiled into firmware.  ELF and JavaScript apps are
external applications.  Built-ins use the same public SDK API and lifecycle as
external apps, but their permission checks always pass.  Neither kind of app
uses private Core headers or ESP-IDF directly.

The one deliberate exception is `modules/selftest`, a built-in diagnostic app
whose entire purpose is validating Core's private implementation (process
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

After boot, Main monitors Core foreground ownership. If UI initialization
succeeded and the foreground stack becomes empty, Main starts the configured
`launcher --gui` command again. Closing or killing the launcher therefore
returns the device to its launcher instead of leaving an unowned display.

`bruce_launcher` is an application, not Core.  It reads `/launcher.json` and
builds a nested menu from it.  Top-level keys are menu labels; values are either
a command string (dispatched as-is to `app_runner__run()` or
`app_runner__run_path()`), a string path starting with `/` that enumerates a
directory (e.g., `"Apps@apps": "/apps"`), or another object defining a submenu.
Labels may end with `@icon-name`; the suffix is hidden from the displayed label
and resolved directly through `icon__get()`. Labels without a suffix have no
launcher icon.
Every submenu automatically appends a `"Back"` entry.  The command strings are
passed exactly as written; the launcher does not append `--gui` automatically.
The launcher starts every command and discovered path in the background. An
application that wants to draw calls `process__to_foreground()` itself; an exact
`--bg` argument lets the application suppress that startup claim.
If `/launcher.json` is missing, the launcher writes a default configuration.

The built-in `apps` module provides the default launcher's application browser.
It enumerates regular `.elf` and `.js` files from `/apps` and `/scripts`, sorts
them by display name, and starts the selected path through the loader registry.
Valid manifests provide display names; files without usable metadata remain
visible under their filename. The browser passes `--gui` to selected apps when
it was itself launched with `--gui` and resumes when the child yields or exits.

## Applications and app_runner

### Entry points

- Every ELF exports `int app_main(int argc, char **argv)`.
- Every built-in registers a uniquely named C function with that same
  signature.  It cannot use the literal global `app_main`, because firmware
  contains multiple modules.
- The built-in ELF loader is additionally exposed as a command named `elf`:
  `elf ./app.elf <args>...` loads the named ELF file and passes the
  remaining arguments to it.  Relative paths starting with `./` are normalized
  to the root directory.  A loaded ELF app with the `execute` permission can
  itself call `app_runner__run_path()` to load another ELF, enabling chains
  such as `elf ./elf_loader.elf ./game.elf` followed by `elf_loader` loading
  `game.elf`.
- A JavaScript file is evaluated first.  If it defines `app_main(argv)`, the
  JavaScript event loop invokes it; it is optional.

### Named execution

The public named-run API is:

```c
int app_runner__run(const char *app_name, const char *arg, bool in_background);
```

It returns a positive Core process ID on success and a negative `BRUCE_ERR_*`
error code on failure.  `arg` is a shell-style argument string (quotes and
backslash escaping). Registered built-ins receive conventional C arguments:
`argv[0]` is the registered command name, `argv[argc]` is `NULL`, and empty or
`NULL` creates `argc == 1`. Loader-resolved ELF and JavaScript applications
retain their loader-defined argument conventions.

Registered built-in command names can be enumerated in registration order with
`app_runner__command_count()` and `app_runner__command_name()`. The returned
names remain owned by AppRunner. The built-in `help` utility uses this registry:
`help` prints all registered commands, while `help <command>` starts the named
command with `--help` and waits for it to finish.

AppRunner owns only the shell-style conversion from command text to `argc` and
`argv`. Built-in modules use the local `components/args` parser when they need
commands, options, or named positional arguments. The parser uses process-owned
Core memory, routes help and diagnostics through Bruce stdio, and reports help,
version, invalid input, and allocation failure as statuses instead of exiting
the application process. Named positional getters return borrowed `argv` strings
and return `NULL` when an optional value is absent. Wi-Fi, Bluetooth, clock,
configuration, IR, NRF24, TCP, WebUI, BNU, notification, image, ELF/JavaScript
loader, and terminal commands all use this parser. Commands that forward an
argument remainder explicitly allow trailing positionals; option parsing can
stop at the first positional so forwarded `--` tokens remain data.

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

which accepts normalized absolute paths and `./` relative paths (which are
mapped to the root directory), but never `.` or `..` components.  It looks up
the loader registered for the path's extension and dispatches to it.  An app with
`execute` may start an ELF, JS, or any other
loader-registered file type from any mounted path this way.  Core does not
discover `/apps/`, `/sdcard/`, or any other folder; launcher and
file-manager apps decide what to enumerate.

Launching another app does not inherit or intersect permissions.  Each target
is its own user-controlled process.

### Loader modules

A loader module owns everything specific to one file format:
decoding/relocating/interpreting its content and running it.  It registers
itself with:

```c
bruce_result_t app_runner__register_loader(const char *extension, int priority,
                                            bruce_loader_run_fn run_fn);
```

`extension` includes the leading dot (for example `.elf`); `priority` breaks
ties when more than one candidate file matches an app name, lower first.
`run_fn` matches `app_runner__run_path()`'s signature e.g., `elf_loader__app_main`.  
Registration happens once at boot, alongside built-in command registration, in main.c, 
before the first named-run or path-run call. A duplicate extension registration is a startup
error, the same as a duplicate built-in command name.

Turning a decoded image into a live process needs more than an ordinary built-in
gets from `app_runner__register()`, so loaders get two extra public
primitives:

```c
int app_runner__spawn_loader_process(const char *permission_key, bool gui_requested,
                                   bool in_background, uint32_t stack_size,
                                   bruce_loader_process_entry_fn entry, void *ctx);
```

```c
const char *manifest__inspect_path(const char *path);
```

```c
bruce_app_inspection_t *manifest__inspect_elf(const char *path);
```

```c
const char *manifest__inspect_javascript(const char *path);
```

```c
bruce_manifest_t *manifest__parse(const char *json, size_t len);
```

`app_runner__spawn_loader_process()` creates a real Core process that calls
`entry(ctx)` on its own stack, wired into the same foreground stack, resource
registry, and permission checks (`permission_key`, e.g. `"game.elf"`) as any
other process.  Its `entry` is the loader's own `elf_loader__app_main`,
`js__app_main`, or equivalent for future formats, so every loader process entry
follows the same naming convention as a regular `app_main`.

`manifest__inspect_path()` is the universal manifest JSON extractor provided
by `core/manifest`.  It auto-detects file format (ELF magic, JS comment
block, or whatever a future format uses) and returns the raw manifest bytes as
a heap-allocated string.  The caller must `free()` the returned string.  The
launcher, file-manager apps, and terminal tools call this one function to
extract manifest JSON from any file uniformly.

`manifest__inspect_elf()` is the ELF-specific full inspection: it validates
the ELF32 header (magic, `e_machine` vs. this build's target), extracts and
parses the `.bruce.manifest` section, and returns a complete
`bruce_app_inspection_t` with kind, parsed manifest, and ABI-warning flag.
The returned structure is heap-allocated; the caller must free it with
`memory__free()`.  The ELF loader module calls this directly at launch time.

`manifest__inspect_javascript()` extracts the optional leading manifest comment
block from a JS file and returns the raw JSON bytes; the caller must `free()`
the result.  `manifest__parse()` parses a JSON string into a `bruce_manifest_t`
that the caller must free with `memory__free()`.

Loader modules do not provide their own inspection — `core/manifest` owns
that capability, so format-aware manifest extraction is not duplicated across
loaders.

A loader module still includes only `core_sdk/...` headers — it gets no
private-header exemption, unlike `modules/selftest`.

Built-in ELF and JavaScript loader modules live under `src/modules/loaders/`,
the same as any other module; they have no special standing over a
third-party loader someone else registers the same way.

### Process lifecycle

Every run creates a Core-managed process.  A normal named run starts foreground;
`in_background` supports autostart and services.  The Core keeps a foreground
stack: foregrounding a new process pushes the prior process; exit or
`process__to_background()` restores the most recent foreground process, falling
back to `bruce_launcher`.

Launcher and terminal front ends use background-first dispatch. Graphical apps
claim foreground with `process__to_foreground()` immediately before interaction;
this also dynamically marks the caller GUI-capable. Built-ins honor exact
`--bg` by not making that startup claim. `--gui` selects a GUI frontend but does
not itself determine foreground ownership.

Backgrounding changes state and physical-input ownership. A background GUI process
is hidden unless the launcher assigns it a compositor tile; hidden drawing is a
successful no-op. Serial access remains independent of foreground state.

Processes are identified by opaque positive IDs in the range `1..INT_MAX`, never
FreeRTOS handles. IDs do not collide with either live processes or retained
completion records, including when ID allocation wraps.
`process__list()` returns read-only snapshots of live processes including state,
stack high-water mark, sampled CPU percentage, and memory/resource statistics.
After resource and display cleanup, Core retains structured completion status in
a fixed 16-entry table. Natural built-in completion is `EXITED` with the integer
entry-point return value. Loader process entries currently return `void`, so a
successful loader return is reported as `EXITED` with code 0. Cooperative `INT`
and `TERM` completion is `TERMINATED` with the delivered signal and exit code 0;
forced `KILL` completion is `KILLED` with signal `KILL` and exit code 0. Shells
translate either signal completion to `$? = 128 + signal`; this translation is a
shell policy and is not stored in `bruce_process_status_t`.

`process__wait()` is non-consuming and succeeds for retained status as well as a
live process that completes before its timeout. `process__wait_status()` atomically
consumes retained status, so exactly one caller succeeds; any caller with the
`process` permission may consume it, without a parent relationship. When the
table is full, the oldest unconsumed, unpinned completion is evicted. Live slots
remain pinned while waiters are attached, preventing slot reuse and lost-event
races.

`INT`, `TERM`, pause, and resume are cooperative. `process__terminate()` sends
`TERM`; `process__kill()` is an explicit forced `KILL` escape hatch. Cross-process
foreground, signal, pause, resume, and kill operations require the `process`
permission; a process may control itself without it. Status consumption always
requires the `process` permission. Cooperative signals wake Core waits and stdio.
The built-in `process signal <int|term|kill> <id|name>` command exposes delivery
to shell users; `process kill <id|name>` remains the direct forced-kill shortcut.
A cancelled shell wait forwards its pending INT or TERM to the child, waits for a
bounded grace period, and force-kills a child that does not stop.

`runtime__sleep(ms)` interrupts a background process when it is foregrounded;
foreground processes sleep for the full duration.  `runtime__delay(ms)` waits the
requested duration regardless of state.  Both are Core APIs; they hide FreeRTOS
from apps. `runtime__now()` returns monotonic milliseconds since boot for
elapsed-time measurement and must not be interpreted as wall-clock time.

### Device state

`device__get_battery()` returns a battery percentage from 0 through 100. The
current Cardputer and StickC Plus2 backends estimate charge from calibrated ADC
voltage; unsupported hardware returns `BRUCE_ERR_UNSUPPORTED` rather than a fake
percentage. `device__get_time()` and `device__get_date()` return configured local
wall time and fail with `BRUCE_ERR_INVALID_STATE` until the system clock contains
a valid date. These APIs expose state only; launcher status-bar rendering remains
module-owned.

### Clock and time

Core keeps the libc system clock in UTC. `clock__get_utc()` reads it directly,
while `clock__get_local()` applies the persisted fixed UTC offset and the
optional manual one-hour DST adjustment. `clock__set_local()` validates a full
calendar value and converts it back to UTC; 12/24-hour format is presentation
policy only. The clock is invalid until its epoch is at least 2020.

`clock__sync_ntp()` performs a bounded SNTP synchronization against
`pool.ntp.org` and reports busy, disconnected, and timeout failures. A station
IP event starts the same synchronization asynchronously only when
`automaticTimeUpdateViaNTP` is enabled. System time is never shifted into a
local epoch, so changing timezone or DST does not rewrite the clock.

The built-in `clock` module provides `show`, monotonic `timer`, and daily
local-time `alarm` actions. `clock --gui` opens the clock face; Select opens its
Timer/Alarm menu and Back exits. Timer and alarm are foreground workflows and
can be cancelled with Back; persistent/background alarms and hardware RTC wake
are outside this initial contract. The built-in `config system clock --gui`
screen and its terminal equivalents configure NTP, timezone, DST, format, and
full manual date/time through the same Core API.

## ELF contract

Every ELF contains a non-loadable `.bruce.manifest` section.  The built-in ELF
loader module reads and validates it before relocation or entry.  The SDK
macro/tool `BRUCE_APP_MANIFEST(...)` emits this section; authors do not
hand-write ELF section attributes.  The SDK build tooling in `elf_apps/tools/`
post-processes linked ELF files with `objcopy` to add the section as
non-allocatable and provides template apps in `elf_apps/examples/`.
For recovery and compatibility, a valid ELF32 file for the current chip may
still launch when its manifest is missing or invalid. The loader logs a warning
and uses the filename, current Core ABI, an 8 KiB stack, and no predeclared
permissions. Invalid ELF headers and mismatching chip architectures remain
launch errors.

The ELF loader module's process entry is `elf_loader__app_main(void *context)`.
It is started by `app_runner__spawn_loader_process()` when an ELF app is launched.
Like any other program entry, it receives the app path, arguments, permissions,
and GUI context through the spawn parameters and its opaque context struct.
Before relocation, Core streams the source file through a 512 KiB `elf_stage`
partition, verifies the staged bytes, and exposes a temporary read-only flash
mapping. Relocation consumes that mapping directly, then releases it before the
ELF process starts, so the complete source file is never retained in RAM. Only
one image can be staged or relocated at a time; this does not prevent already
relocated ELF processes from running concurrently.

The manifest is canonical UTF-8 JSON with fixed field order and no
insignificant whitespace.  Permission array order is author-controlled, but
duplicates are invalid.  Required fields are `appName`, `appIcon`,
`coreAbiVersion`, and `stackSize`; `permissions` is optional and defaults to an
empty array.  A complete example is:

```json
{
  "appName": "Example app",
  "appIcon": "<Base64 128-byte bitmap>",
  "coreAbiVersion": 3,
  "permissions": ["wifi", "http"],
  "stackSize": 8192
}
```

- `appName` is the launcher/process-manager display name.  The filename is the
  command identity; there is no `appId`.
- `appIcon` decodes to exactly 128 bytes: 32×32 monochrome, row-major,
  most-significant-bit leftmost.  A 1 uses launcher foreground colour; 0 uses
  launcher background colour.
- `stackSize` is bytes, constrained to 4–16 KiB.  SDK tooling uses 8 KiB by
  default.
- An omitted or empty `permissions` array gives a safe zero-permission ELF
  fallback.
- A Core ABI mismatch is reported by `manifest__inspect_elf()` but is not yet
  enforced with a warning/confirmation dialog.
- The ELF header, not the manifest, is authoritative for architecture; a
  mismatching chip architecture is rejected.

Ed25519 signing is deferred.  Canonical manifest bytes and the ELF
loadable-content hash are the future signing inputs.

Manifest inspection — extracting the `.bruce.manifest` section bytes from an
ELF file, checking `e_machine`, and parsing/validating the JSON — is handled
by `manifest__inspect_elf()` in `core/manifest`, not by the ELF loader
module.  The ELF loader calls it at launch time to read and validate the
manifest.  Other programs (launcher, file manager, terminal) call the
universal `manifest__inspect_path()` to extract raw manifest JSON from any
file format without involving the loader module.

The ELF loader module's `elf_find_sym()` resolves only the documented public
SDK symbol allowlist it maintains.  It never resolves ESP-IDF, private Core,
or arbitrary libc symbols. Standard heap imports (`malloc`, `calloc`,
`realloc`, and `free`) resolve to the process-owned `memory__*` API, while console
imports such as `printf` resolve to routed Bruce stdio. A trusted native app
can still embed a custom allocator, which is unsupported rather than a hard
sandbox violation.

The allowlist also includes the narrow set of GCC floating-point helper symbols
needed by freestanding C applications. These resolve to the firmware's libgcc
implementation and are compiler runtime support, not ESP-IDF or Core APIs.

## JavaScript contract

The built-in JavaScript loader module evaluates the script and owns its
mQuickJS event loop.  JavaScript timers and `runtime.main()` remain
JavaScript-runtime features; they are not moved into Core.  The loader
allocates VM/context memory with `memory__malloc()`.

The JS loader module's process entry is `js__app_main(void *context)`.  It is
started by `app_runner__spawn_loader_process()` when a JS script is launched.
Like any other program entry, it receives the script path, arguments,
permissions, and GUI context through the spawn parameters and its opaque
context struct.

A JS file may start with the same canonical manifest in a comment block.  It
is optional.  An unmanifested script starts with zero grants, uses its filename
as display name, a generic icon, and the mQuickJS default process stack.
Manifest inspection — detecting the leading JS comment block, extracting the
raw JSON, and parsing/validating it — is handled by the universal
`manifest__inspect_path()` in `core/manifest` (raw extraction) plus
`manifest__parse()` (parsing), not by the JS loader module.

Preserve the existing JS surface as much as possible: `wifi.scan()`,
`dialog.choice()`, `display.*`, `runtime.*`, `serial.cmd()`, and similar APIs
remain.  Their bindings are replaced internally to call the public Core APIs;
they are not renamed to a new `bruce.*` namespace.

`runtime.main()` is retained for now even though optional `app_main(argv)` is
the normal JS lifecycle entry.  `serial.cmd(command)` delegates to the same
`serial_commands`/AppRunner parser and requires `execute`.

## Terminal and stdio sessions

`terminal` is a GUI-by-default built-in. It owns one persistent background
`shell` child, displays the child's captured stdout/stderr, and routes input
bytes to its stdin. Shell variables therefore persist for the terminal lifetime.
Graphical commands explicitly claim foreground and return to the terminal when
they exit. Back terminates the shell before closing. The physical
`serial_commands` frontend runs the same interactive shell language. The
configured ESP-IDF console transport and its input driver remain Core-owned.

Interactive shell input supports insertion and deletion at the cursor, Left,
Right, Home, End, and Up/Down history navigation. The physical console consumes
ANSI/VT100 escape sequences; the GUI terminal translates normalized input events
to those same byte sequences and renders the shell's inline prompt. Non-empty interactive commands
are appended to `/shell_history`. History entries are fetched from storage only
when navigating, rather than retained in a RAM history ring, and the active file
rotates to `/shell_history.old` at 64 KiB. The GUI transcript interprets ANSI SGR
foreground colors and reset/bold variants, strips OSC/control sequences, and
handles erase-screen/erase-line sequences within its bounded transcript. The GUI
terminal forwards character and navigation-key bytes directly to its interactive
shell, which renders and edits its prompt inline with command output just like
the physical console. Interactive children such as SSH use that same terminal.

The built-in `shell` module owns command-language parsing and execution. It
supports whitespace-delimited words, literal single quotes, expandable double
quotes, backslash escaping, `NAME=value`, `$NAME`, `${NAME}`, `$?`, token-boundary
comments, and left-to-right `;`, `&&`, and `||`. A single bounded pipeline may
feed `text` from `echo` or an external producer, for example
`cat /notes.txt | text` or `cat /notes.txt | text /copy.txt`. Its bounded variables persist
across interactive input and script lines. Builtins are `echo`, `true`, `false`,
`set`, `unset`, `export`, `clear`, `exit`, and `help`; other names and absolute/`./` paths
launch through AppRunner in Core-background mode and are synchronously reaped.
The `.sh` loader invokes this built-in for absolute scripts. Command
substitution, globbing, subshells, functions, positional parameters, general or
chained pipelines, and redirection are deferred. Unsupported pipeline forms and
unquoted, unescaped `<` and `>` are explicit syntax errors.

The built-in `text` editor opens `.txt`, `.json`, and `.conf` paths through the
loader registry. It also accepts the shell's exact-size piped stdin handoff,
prompts for a destination when unsaved piped text is saved, and limits editable
content to 32 KiB. `text -r <path>` (`--read-only`) and piped input with the same flag
disable mutation and saving, providing a lightweight `less`-style viewer. The
editor binds Ctrl+S to save and Ctrl+X to exit, with both shortcuts shown in its
footer; read-only mode exposes only Ctrl+X.

The built-in BNU (Bruce is Not Unix) module provides the direct commands
`pwd`, `cd [directory]`, `ls [path]`, `mkdir <directory>`, `touch <file>`,
`cat <file>...`, `free`, and `top`. BNU keeps a shell working directory for
relative storage paths; it is independent of libc process cwd. `cat` streams
files unchanged to app-visible stdout. `free` reports Core-provided internal RAM
and PSRAM heap statistics. `top` reports the same system heaps plus CPU usage,
stack high-water bytes, and tracked heap usage for each Core-managed process.

Core owns bounded, process-owned stdio sessions. A session owner may route newly
created child processes to the session, drain captured output, and enqueue input.
The route is inherited atomically during process creation and becomes the
child's default route for its own children, so a terminal-routed shell passes the
same session to command grandchildren. The owner should clear its launch route
immediately after launch. Session resources are released automatically with
their owner; output overflow drops the oldest captured bytes, while input
overflow returns `BRUCE_ERR_RESOURCE_LIMIT`.

App-visible command output uses `bruce_stdio_write()` or
`stdio__printf()`. These functions write to the calling process's routed
session, or to the physical serial console when no session is routed. Normal
libc `printf()` and Core logging remain diagnostic serial output and are not
captured, preventing unrelated subsystem logs from polluting terminal output.

## Permissions

Permissions are coarse-grained.  The current vocabulary is:

```
http, wifi, bt, gps, rf, input, gpio, ir, rfid, microphone,
hid, execute, process, storage, config, serial
```

`gpio` includes raw GPIO, I²C, and SPI. `rf` includes Sub-GHz, LoRa, and NRF24. `audio` is not
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

Built-in module processes are implicitly granted every permission.  External
processes are checked inside each protected Core API.

Prompts are not reentrant: while a task is inside a permission prompt dialog,
any nested `permission__check()` from that same task (e.g. the dialog reading
theme colors through `config__*`) is denied immediately without a second
dialog, so prompting can never recurse.

`http__request()` and `http_server__*` need `http`; neither implies `wifi`. Wi-Fi state control,
credentials, and raw TCP sockets need `wifi`. `input__inject()` needs `input`; process control of
another process needs `process`; starting a built-in command or path needs `execute`.

`http__request()` accepts at most 64 KiB of response body by default. Callers may set
`max_response_bytes` to another non-zero limit. A response that exceeds the selected limit fails
with `BRUCE_ERR_RESOURCE_LIMIT`. Setting `on_response_chunk` delivers body data synchronously as it
arrives instead of retaining it; callback data is borrowed for the callback duration, and the
successful response reports the delivered byte count with a null body. Buffered response bodies
are NUL-terminated, while `body_len` remains authoritative. Core retains at most 32 response
headers and 4096 bytes of header text; omitted excess headers do not fail the request. The retained
headers and buffered body share one process-owned allocation released by `http__response_free()`.

## Dialog and process interaction

`dialog__*` is renderer-agnostic.  `dialog__choice()` displays a GUI choice
for processes launched with `--gui`, or a terminal choice such as:

```
1. yes
2. no
pick: _
```

GUI callers may provide render parameters with per-edge padding, optional
title/footer bars, choice text size, and content colors. The choice renderer
derives its visible row count from the remaining padded viewport and scaled
row height. An optional render callback runs inside the dialog frame, with a
caller-selected refresh interval, so surrounding UI outside the padded
viewport can remain current while the choice is open. Terminal rendering
ignores these parameters.

app_runner records the initial `--gui` launch context in process-local storage
before launch-time permission checks, but leaves the argument in the app’s
`argv`.  A background serial-monitor-style process decides its own behavior;
there is no separate dynamic process interaction-mode API.

Retain the JS `dialog.message`, `info`, `success`, `warning`, `error`, and
`choice` APIs as wrappers.  Keep `dialog.pickFile()` as a Core renderer-neutral
API: it requires `storage` and hides `/bruce.json` and `/permissions.json`.
Do not migrate `dialog.viewFile()`.  `dialog__create_text_viewer()` returns an
opaque viewer ID with draw, scroll, set-text, and close operations; it is a
tracked resource.

## Memory and resource ownership

ELF apps never receive libc `malloc` or `free`.  They use
`memory__malloc()`/`memory__calloc()`/`memory__realloc()`/`memory__free()`.
Core allocates ELF images, BSS, loader bookkeeping, and JS VM/context memory
through this same process-owned allocator.

Every process has one universal resource registry in thread-local storage.  Core
services register opaque handles and Core-owned cleanup callbacks for memory,
files, sockets, viewers, radios, and similar resources.  Apps cannot register
arbitrary callbacks.  At normal exit or kill, Core reads the registry before
the process disappears and cleans resources in reverse acquisition order.

## Bluetooth

Bluetooth Core owns the single ESP-IDF controller and Bluedroid host instance.
`bluetooth__scan_ble()` performs a bounded, synchronous BLE advertisement scan,
deduplicates devices by address, and returns Core-independent result records in
descending RSSI order. Bluetooth APIs require `bt`; built-in apps receive that
permission implicitly.

Classic HID hosting is exposed separately through `bluetooth_hid__*`. It is
available only when the SoC and build support Classic Bluetooth and otherwise
returns `BRUCE_ERR_UNSUPPORTED` (notably on ESP32-S3). One Classic HID keyboard
or gamepad may be connected at a time. The connection belongs to the process that
opened it and closes during process cleanup. The `bluetooth_hid_app` built-in can
remain in the background while its device feeds the foreground app.

Keyboard boot reports become key press/release events, including normalized
navigation, Select, and Back codes. Common gamepad reports expose four signed
axes, a hat mapped to directional events, and twelve stable button codes through
the public input vocabulary. Descriptor-specific report layouts that do not
match that common format are not guessed. All accepted HID reports enter Bruce
only through `input__inject()`; modules never receive ESP-IDF Bluetooth handles.

## Input, display, storage, and Config

Physical buttons, touch, keyboard, and encoder input go only to the effective
foreground process. Blocking reads carry an input-owned foreground epoch and are
revoked immediately on handoff, including an A-to-B-to-A transition. Timeout
budgets span internal wakes. `input__inject()` accepts a normalized event with type, action,
code, value, timestamp, and source process ID, allowing Bluetooth, GPIO, and I²C
adapters to feed the same pipeline.

Raw GPIO is exposed through `gpio__configure()`, `gpio__read()`, and
`gpio__write()`. Modes and pin capabilities are validated against the selected
SoC, and every public operation requires `gpio`.

I²C master buses are opened with `i2c__open()` using an explicit or
automatically selected controller, SDA/SCL pins, clock, and pull-up policy.
Compatible opens share one Core-owned hardware bus. Handles are process-owned,
close automatically, and support 7-bit probe, write, read, and repeated-start
write/read transactions of at most 4096 bytes. Reserved addresses outside
0x08 through 0x77 are rejected. A probe NACK is `BRUCE_OK` with `present=false`;
transaction NACK is `BRUCE_ERR_NOT_FOUND`.

SPI devices are opened with `spi__open()` on the board's external
`SPI3_HOST`. The first device selects the SCK/MISO/MOSI tuple; additional
devices may attach while that tuple matches and have independent CS, mode, and
clock settings. Device handles are process-owned, close automatically, and allow
bounded full-duplex transfers of at most 64 bytes without DMA. The display continues to
own `SPI2_HOST` and is never attached through this API. Core hardware drivers
use trusted private GPIO/bus entry points so their capability-specific
permission remains authoritative.

Display Core owns one RGB565 framebuffer and transfers run synchronously in
the caller's task: `display__present()` (and the `display__flush()`
implicit-frame path) pushes the viewport rect to the panel before returning,
serialized by the display lock, so transfers never race with drawing or with
each other. GUI processes draw in local coordinates into a fullscreen
foreground viewport, one of up to four launcher-assigned non-overlapping
tiles, or a hidden zero-sized viewport. `display__begin_frame()` leases the
viewport through the completion of `display__present()`; tile rows are packed
into a shared DMA scratch row buffer. A full-screen `display__present()` with
the default configuration streams the framebuffer directly over DMA
(`displayDmaFramebuffer` in `bruce.json`, default true); any partial rect,
overlay composition, or a `false` setting falls back to row-packed transfers.
Text and cursor state are process-local, rotation is global, and no resize event is
emitted. Drawing primitives include legacy-compatible circular arcs whose zero
angle is at six o'clock and increases clockwise.
`display__draw_bitmap_scaled()` blits a 1bpp MSB-first bitmap of any size into
a destination rectangle with nearest-neighbor scaling and transparent clear
bits, using only integer math, and is the preferred way to draw filled icons.
`display__flush()` provides an implicit-frame compatibility path.

Icon Core stores a small set of built-in 24x24 Material Design Icons as
pre-rasterized 1bpp bitmaps (72 bytes each, MSB-first) in read-only firmware
memory — the cheapest filled-icon representation: no parsing, no floating
point, no working RAM at draw time. `icon__get(name)` returns a Core-owned
`bruce_icon_t` (never free it) for recognized names `wifi`, `ble`, `remote`,
`handheld`, `folder`, `files`, `terminal`, `clock`, `settings`, `selftest`,
and `apps`. Unknown names and `NULL` return `NULL`. The intended consumer is
`display__draw_bitmap_scaled()`; the bitmaps are generated from the MDI source
assets at development time.

Image Core decodes JPEG, PNG, and the first frame of GIF data from memory or a
Core storage path into the caller's viewport. `image__draw_memory()` and
`image__draw_path()` optionally fit without upscaling, preserve aspect ratio,
center relative to caller coordinates, and composite transparency over a
caller-selected RGB565 background. They update the framebuffer but leave frame
presentation to the caller. The image loader registers `.jpg`, `.jpeg`, `.png`,
and `.gif` case-insensitively; its viewer fits, centers, presents, and remains
open until input. File manager image viewing and terminal/serial direct paths
use this same loader. GIF animation is not part of this initial contract.

Core also owns one transient notification overlay. `notification__push()` and
`notification__dismiss()` are unrestricted, last-writer-wins operations; text
is copied into fixed storage and duration is clamped to 250 through 30000 ms.
The overlay is composed only into transfer scratch rows, so it never changes
application framebuffer pixels. Push and dismiss repaint the overlay rect
immediately unless a frame is mid-flight over it, in which case the overlay
appears on that app's next `display__present()`; expiry is lazy and the
overlay disappears on the next transfer that overlaps its rect.

The unrestricted status-icon service is a separate runtime-only global keyed
registry. Any process may replace or remove any key, including one created by a
different process. Entries survive producer exit, list in lexicographic key order,
and every effective mutation increments a revision. Core never renders these
1bpp icons or reserves a status bar; the launcher and interested applications
list and draw them themselves. Wi-Fi publishes the `core.wifi` icon after a
station obtains an IP address and removes it when that station disconnects.

`storage` grants access to Core `storage__*` APIs.  Public file handles are
opaque IDs and are closed automatically at process teardown.  The only v1
protected paths are `/bruce.json`, `/permissions.json`, and their atomic-write
temporary files; all other mounted paths are usable by a storage-granted app.
`storage__mkdir()` creates one directory at a time through the same path and
permission policy and succeeds when that directory already exists. Public
remove, same-volume rename, and volume-usage queries enforce the same normalized
path and protected-configuration policy.

Raw IPv4 TCP uses process-owned opaque handles through `tcp__connect()`,
`tcp__listen()`, `tcp__accept()`, `tcp__read()`, `tcp__write()`, and
`tcp__close()`. Connect accepts hostnames, all blocking operations have caller-
supplied timeouts, and EOF is reported as a successful zero-byte read. Handles
are restricted to their owning process and close automatically at process teardown.
The built-in `tcp` terminal app provides client and sequential single-client
listener modes. It forwards raw stdin bytes to the socket and prints received
bytes to stdout; Ctrl+] closes the active mode. The terminal waits for a
launched foreground command to exit, preventing its prompt from competing for
stdin.

SSH client sessions are Core-owned opaque handles backed by the managed
`wolfssl`/`wolfssh` components. They require the independent `ssh` permission, belong
to the creating process, and close automatically at process teardown. The
public API exposes handshake, SHA-256 host-key fingerprint retrieval and
verification, password or ECDSA P-256 key authentication, keypair generation,
PTY shell open/resize, and nonblocking
channel byte I/O with caller-supplied timeouts. Core refuses authentication
until `ssh__verify_host_key_sha256()` succeeds; callers may not silently bypass
host-key verification. Library sessions, channels, and native sockets remain
private to Core and are never exported through the ELF SDK.
The built-in `ssh-keygen` command defaults to an ECDSA P-256 key at
`/.ssh/id_ecdsa`; `--type ed25519` generates an unencrypted OpenSSH Ed25519
private key at `/.ssh/id_ed25519` instead. Both write an OpenSSH-compatible `.pub` file. Host keys
are stored in `/.ssh/known_hosts` (the former `/ssh_known_hosts` is migrated on
first use). The SSH app reads `/.ssh/config` global and matching `Host` sections,
including `HostName`, `User`, `Port`, and `IdentityFile`; command-line values take
precedence. The `ssh --identity <path>` option authenticates with such a private
key after the same mandatory host-key verification flow. Authentication accepts
unencrypted ECDSA P-256 SEC1 PEM and OpenSSH Ed25519 private keys; the default identity
is also selected automatically when it exists unless `--password` is supplied.

Inbound HTTP is a generic Core service exposed through `http_server__*`. A
caller with `http` permission supplies bounded fixed or dynamic route
descriptors. Core copies fixed metadata and bodies, owns the device-wide
ESP-IDF HTTP server, and provides opaque request, receive, header, response, and
chunked-transfer operations without exposing ESP-IDF handles. Dynamic handlers
run concurrently in a bounded worker pool so file transfers do not block quick
requests; callbacks and retained contexts must therefore be thread-safe. The API contains
no Wi-Fi, WebUI, asset, or application policy. The built-in `webui` module
reuses an active Wi-Fi connection or selects a configured station/access point,
serves generated gzip assets from `embedded_resources/web_interface`, and owns
authenticated file management, uploads, command/navigation input, credentials,
screen capture, status, and reboot routes. WebUI sessions and credentials remain
permanently unavailable to external ELF/JS processes.

`config` is one Core-owned singleton exposed through type-safe field APIs such
as `config__get_bright()`, `config__get_theme_path()`, and
`config__set_sound_enabled(true)`. Scalar getters return values directly;
string, string-array, and credential getters return read-only pointers into the
singleton without allocating or copying. Those pointers remain valid until the
configuration is changed, loaded, or reset and callers must not free them.
`config__get_startup_apps()` returns the singleton's `startupApps` array and
count. `config__add_startup_app()` appends a key only when it is not already
present, and `config__remove_startup_app()` removes a key while preserving the
order of the remaining entries. `hotkeys` is a bounded key-to-action object.
`displayDmaFramebuffer` defaults to true and is applied at boot. When false,
the full compositing framebuffer is ordinary internal memory and display
updates are copied through a small DMA-capable row buffer instead of issuing a
full-frame DMA transfer.
Each action is an AppRunner command line: its first token selects a registered
command or loader path and the remaining text is passed as its arguments. The
default `alt + tab` chord runs `process switch next`, cycling foreground focus to
the next background GUI process. The same operation is available through
`process__switch_next()`. The built-in `process` utility supports
`process switch <next|prev|id>`, `process preview`, and `process kill <id|name>`.
`process preview` claims foreground and tiles up to four background GUI processes at a
time through the compositor (`display__set_tiles()`); arrows move the selection
across the grid (paging when more than four qualify), Select foregrounds the
highlighted process, and Back clears the tiles and exits. Kill names are exact
matches and ambiguous duplicate names fail without killing either process. Setters
validate and atomically persist immediately. The following
values are permanently protected from ELF and JS, even with `config`:
`wifiAp.ssid`, `wifiAp.pwd`, `webUI.pwd`, `wifiCredentials`, `wifiMAC`, and
`webUI.user`. Built-ins may use those APIs.

Core registers internal LittleFS at runtime over all sector-aligned flash after
the final static partition. The factory partition is 2.5 MiB and the following
512 KiB `elf_stage` partition ends at the former 3 MiB factory boundary, so the
LittleFS start address remains unchanged. The flashed partition table therefore remains
usable on 4, 8, 16, and 32 MiB devices while LittleFS consumes the available
remainder reported by the flash driver. Existing filesystems at the same start
address grow on mount; Core formats only when the LittleFS metadata area is
erased, so a failed migration mount does not silently erase nonblank data.

`ir` grants access to synchronous ESP-IDF RMT infrared capture and transmit
through `ir__receive()`, `ir__transmit()`, and `ir__transmit_raw()`. Captures
are returned as Bruce/Flipper version-1 IR records. Decoded capture recognizes
NEC; NEC, NECext, Samsung32, and Sony SIRC variants can be transmitted. Unknown
captures can be read as raw 38 kHz timings. `ir__transmit_record()` replays an
in-memory capture so learning workflows can test it before saving.
`ir__transmit_file()` replays version-1 `.ir` files and
also requires `storage`, since it uses process-owned public storage handles. One
transmission is always made and `repeats` specifies additional transmissions.
IR GPIOs are board defaults configurable through Kconfig.
The built-in Infrared app provides regional TV power-code runs, RMT-backed
basic/enhanced/sweep/random/empty jammer patterns, custom learning, and TV, AC,
fan, sound, and LED-strip quick-learning templates. Learned remotes use the
version-1 format under `/BruceIR`; filename collisions may be numbered,
overwritten, renamed, or cancelled.

`rf` grants access to the NRF24 Core API. `nrf24__probe()` checks physical chip
presence, channel operations cover the hardware range 0 through 125, and
`nrf24__scan()` returns bounded counts from the radio's RPD threshold detector.
RPD results indicate 2.4 GHz activity only; they are not calibrated RSSI and do
not decode packets. Core owns the radio and serializes synchronous operations.
NRF24 attaches to the shared external SPI Core bus on `SPI3_HOST`, leaving the
display-owned `SPI2_HOST` untouched, with
board-specific SCK, MISO, MOSI, CS, and CE defaults configurable through
Kconfig. The built-in NRF24 app exposes status and passive spectrum scans in
both launcher GUI and terminal forms. The v1 API intentionally excludes packet
transmit, MouseJack, and constant-carrier jamming.

## Public SDK and migration rules

Maintain two header layers:

- `src/core/` contains private Core implementation headers and ESP-IDF
  details; only Core source may include them.
- `src/core_sdk/` contains the public Core SDK headers used by every built-in
  module and external ELF app.  SDK callers include them with their full,
  unambiguous namespace, for example `"core_sdk/app_runner.h"`.

`core_sdk/loader.h` declares the loader registry
(`app_runner__register_loader()`, `app_runner__run_path()`,
`app_runner__spawn_loader_process()`) that any loader module uses.  Manifest
inspection is provided by the universal `manifest__inspect_path()` in
`core_sdk/manifest.h` / `core/manifest/`; it returns a heap-allocated raw JSON
string that the caller must `free()`, auto-detects file format, and replaces
per-loader inspection functions so the launcher, file manager, and any other
tool can inspect any file uniformly.  Built-in ELF and JavaScript
loader modules live under `src/modules/loaders/elf/` and
`src/modules/loaders/js/`, like any other built-in module, not under
`src/core/`.

The component exports the `src/` include root only so those `core_sdk/...`
includes resolve.  Built-in modules include only `core_sdk/...` headers;
their privilege is policy, not access to private C declarations.  The single
exception is the `modules/selftest` diagnostic built-in described under
"Apps and modules own", which may include `src/core/` headers directly; it is
excluded from the SDK-only compile smoke targets in `src/CMakeLists.txt` that
build the other built-ins against the public `core_sdk/` include namespace.  New public
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
that API.  Do not delete or change BrucePIO_legacy; use it only as the behavioral
reference.  Wi-Fi is the first complete vertical slice.
