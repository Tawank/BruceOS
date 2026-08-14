# WASM Core SDK Bindings Plan

## Goal

Allow a module such as `src/modules/clock/clock_app.c` to build and run as
WebAssembly from the same source used by the built-in and ELF targets.

The public C API remains the existing `src/core_sdk/` API. This work must not
change declarations, types, or names in `src/core_sdk/`. Built-in modules and
ELF applications must not require source or build changes. Where WASM's linear
memory makes a native ownership/accounting guarantee impossible, document the
WASM-specific behavior in the loader contract rather than changing the public
header.

All native/WASM pointer translation belongs to the WASM binding under
`src/modules/loaders/wasm/`. WASM-only compilation support may be added to the
external-app build tools, but it must consume the existing Core SDK headers
rather than introduce another public API.

The first acceptance target is Clock. This plan does not require exposing the
entire Core SDK before Clock can be enabled for WASM.

## Why A Binding Is Required

ELF applications execute in the ESP address space, so the ELF loader can export
a Core function address directly. A WASM pointer is instead a `uint32_t` offset
inside one module's linear memory. Passing that offset directly to a Core
function as an ESP pointer would permit invalid memory access and normally
crash or corrupt the firmware.

The WASM binding therefore has two cases:

- Scalar-only calls use small forwarding wrappers.
- Calls containing pointers validate each guest range, translate or copy it,
  call the unchanged Core function, and serialize outputs back into guest
  memory.

Libc functions are not Core functions. String comparison, parsing, allocation,
and formatting must execute inside WASM so their pointers remain guest
pointers.

## Non-Goals

- Do not edit `src/core_sdk/*.h` for WASM attributes or alternate signatures.
- Do not add `wasm__*` public Core APIs.
- Do not alter the ELF symbol table as part of this work.
- Do not expose native ESP libc functions to WASM.
- Do not pass native pointers to the guest, even when both happen to be 32-bit.
- Do not make every Core SDK capability available in this first slice.
- Do not enable APIs with incompatible semantics, such as returning a direct
  mapping of native external memory, merely to claim complete SDK coverage.
- Limit the first guest build integration to C sources. Reject C++ WASM inputs
  with a clear build error until the SDK has a tested C-linkage policy; the
  current public headers do not consistently provide `extern "C"`.

## Existing Code To Preserve

- `src/modules/loaders/wasm/wasm_bruce_host_adapter.c` registers imports in the existing
  `bruce_sdk` module. Keep that module name and extend its symbol table.
- `src/modules/loaders/wasm/wasm_loader_app.c` owns WAMR initialization,
  instance limits, process startup, argument copying, and teardown.
- `src/core_sdk/` remains the authoritative source API.
- `components/args/args.c` remains the one argument-parser implementation.
- `native_apps/tools/build_apps.py` and `native_apps/tools/build_modules.py`
  continue to build the same application sources for each external target.

The existing WASM loader necessarily includes the public WAMR component API in
addition to `core_sdk` headers. During implementation, clarify in `AGENTS.md`
and `ARCHITECTURE.md` that loader modules may include the public API of the
third-party runtime they adapt, while they still may not include Core-private
or unrelated ESP-IDF headers. This documents the existing WAMR dependency; it
does not grant general private-Core access to modules.

## Proposed File Layout

Keep host and guest sides visibly separate even though both are WASM-specific:

```text
src/modules/loaders/wasm/
  wasm_loader_app.c              existing WAMR/process loader
  wasm_bruce_host_adapter.c      host import wrappers and registration
  wasm_bruce_host_adapter.h      host registration declaration
  wasm_bruce_abi.h               private wasm32 layouts and limits
  wasm_bruce_guest_adapter.c     compiled into WASM apps, never firmware
```

`wasm_bruce_guest_adapter.c` is not added to `src/CMakeLists.txt` firmware sources. The
two Python WASM build paths compile it into each WASM application.

If `wasm_bruce_guest_adapter.c` becomes unwieldy, split it under
`src/modules/loaders/wasm/guest/`. Do not split it before that is useful.

## Guest Adapter

### Purpose

Application source includes the normal `core_sdk/*.h` headers. The guest
adapter defines the supported public functions with those exact signatures.
Each definition calls an internal function carrying Clang's explicit WASM
import attributes.

Conceptual example:

```c
__attribute__((import_module("bruce_sdk"), import_name("clock__get_local")))
extern int32_t wasm_import__clock_get_local(uint32_t out_offset);

bruce_result_t clock__get_local(bruce_clock_datetime_t *out) {
    return wasm_import__clock_get_local((uint32_t)(uintptr_t)out);
}
```

The internal C identifier differs from the import name, so the adapter can
define the public function without a symbol collision. The import remains
named `bruce_sdk.clock__get_local`, matching the host registration.

Add guest definitions for the already-supported imports as well as the new
Clock slice. This ensures the build does not depend on wasm-ld's default module
name for unresolved symbols.

### Guest-Local Functions

Some public functions cannot cross the native boundary with their C ABI and
must be implemented inside the guest:

- `memory__malloc`, `memory__calloc`, `memory__realloc`, and `memory__free`
  delegate to the guest libc allocator. WAMR instance teardown remains the
  final cleanup boundary.
- `stdio__printf` and `stdio__vprintf` format in guest memory with
  `vsnprintf`, then call the imported `stdio__write` function.
- Standard libc functions such as `snprintf`, `sscanf`, `strcmp`, `strlen`,
  `memcpy`, `strtol`, and `strtod` are linked into the module and are not Bruce
  imports.

Preserve the documented `stdio__printf` result: return the formatted byte count
on success and a negative `BRUCE_ERR_*` result when output fails. Handle
`va_list` with `va_copy`; do not reuse a consumed list.

Guest allocator calls cannot be represented as individual Core-tracked native
allocations because Core pointers are not addressable from linear memory.
Document that WASM allocation is accounted and reclaimed at the WAMR instance
level, not as individual `memory__*` resources in process snapshots. Do not
pretend that native allocation pointers can preserve the tracking semantics.

### Argument Parser

Compile the existing `components/args/args.c` into WASM artifacts. Do not create
host wrappers for `ap_*`.

This is important because `ArgParser *`, parser-owned borrowed strings, parser
arrays, and command callbacks naturally remain valid when the parser lives in
guest memory. Host parser wrappers would require an object-handle registry,
guest copies of every borrowed result, and callback trampolines.

The existing parser source already depends only on normal libc plus
`memory__*` and `stdio__*`; the guest-local implementations above satisfy those
dependencies. Build the complete parser rather than selecting only the calls
currently used by Clock.

## Freestanding Libc And Linker Work

The current `--target=wasm32 -nostdlib -ffreestanding` command cannot compile
Clock because the toolchain has no `stdio.h`, `stdlib.h`, or `string.h` target
headers. A complete implementation must provide a wasm32 freestanding libc
sysroot/archive, not native libc imports.

Use a wasm32 libc capable of providing the pure in-module functionality needed
by Clock and `components/args/args.c`. A WASI SDK libc may be used only when the
linked result has no `wasi_snapshot_preview1` imports. An embedded wasm32 libc
such as picolibc is also acceptable. The deciding acceptance condition is the
final import set, not the libc vendor.

Update both build scripts:

- `native_apps/tools/build_apps.py`
- `native_apps/tools/build_modules.py`

Required changes:

1. Accept a configured WASM compiler/sysroot rather than assuming the host
   Clang resource headers are a usable libc.
2. Compile `wasm_bruce_guest_adapter.c` for every WASM app.
3. Compile `components/args/args.c` for modules/apps that use the parser. It is
   acceptable initially to include it in every WASM artifact if dead-code
   elimination removes it when unused.
4. Link the guest libc and allocator.
5. Keep exporting only `main` and the module memory.
6. Replace unrestricted unresolved-symbol acceptance with an explicit check of
   the final import section.
7. Fail the build if an import module is not `bruce_sdk`, or if a symbol is not
   in the supported Bruce WASM import allowlist.
8. Parse the type section and each imported function's type index. Require the
   exact parameter/result signature registered in `s_native_symbols`; reject
   duplicate imports, invalid type indices, non-function imports, multi-result
   mismatches, and malformed sections.
9. Continue applying the `bruce.manifest` custom section after successful
   linking and import validation.

The import checker should be implemented in Python by reading the WASM import
section, so builds do not depend on `wasm-objdump` being installed. Reuse a
small bounded u32 LEB128 reader similar to the loader preflight code. Reject
malformed binaries as well as unexpected imports.

Do not silently allow WASI imports. WAMR intentionally has WASI disabled in
this firmware.

## Host Binding Helpers

Extend `wasm_bruce_host_adapter.c` with a small set of private helpers. Keep pointer
signatures as WAMR `i32` values and explicitly validate every range before
calling `wasm_runtime_addr_app_to_native()`.

Required helpers:

- Required byte span: reject a zero offset, zero/invalid required size, address
  overflow, or a range outside linear memory.
- Optional byte span: accept offset zero only where the public API permits
  `NULL`; otherwise apply required-span behavior.
- Required C string: reject offset zero and use WAMR's bounded guest-string
  validation before translation.
- Optional C string: map offset zero to native `NULL`; otherwise validate the
  complete NUL-terminated string.
- Array span: reject `count * element_size` overflow before validating the
  resulting range.
- Little-endian load/store helpers for `uint16_t`, `uint32_t`, and `uint64_t`.

Do not dereference a translated pointer until validation succeeds. Do not cast
unaligned guest addresses to native structure pointers. Do not retain a
translated guest pointer after the imported call returns.

WAMR's `wasm_runtime_validate_app_addr()` and
`wasm_runtime_validate_app_str_addr()` set an out-of-bounds exception when they
reject an address. A normal bad SDK argument must remain a recoverable Bruce
error, so each helper must call `wasm_runtime_clear_exception()` before
returning validation failure. Add an executable test proving the guest can
continue after receiving `BRUCE_ERR_INVALID_ARGUMENT` from a bad pointer.

Use `BRUCE_ERR_INVALID_ARGUMENT` for malformed guest offsets, strings, sizes,
or nested records. Use `BRUCE_ERR_RESOURCE_LIMIT` when a valid wasm32 request
cannot represent a native result or exceeds an explicit binding limit.

## Private wasm32 Layouts

Declare fixed layouts and offsets in `wasm_bruce_abi.h`. These are private to
the WASM loader and guest adapter; they are not new Core SDK types.

Serialize fields individually. Do not `memcpy` a native Core structure into
guest memory. Native and wasm32 layout may currently appear equal, but native
`size_t`, enum representation, Boolean layout, alignment, or later compiler
changes must not become an implicit ABI.

For each guest-visible public structure used by the first slice, add guest-side
`_Static_assert`s for `sizeof` and every field offset. The host wrapper writes
the corresponding explicit byte offsets.

### Clock Date/Time

The wasm32 public structure must expose the fields from
`bruce_clock_datetime_t`:

```text
year    u16
month   u8
day     u8
hour    u8
minute  u8
second  u8
padding zeroed through sizeof(bruce_clock_datetime_t)
```

Call `clock__get_local()` with a native stack object. Only after it succeeds,
clear the validated guest output and store each field.

### Process Snapshot

Call `process__snapshot()` with a native stack object, then encode every public
field into the wasm32 layout expected by `bruce_process_snapshot_t`:

- Numeric IDs, state, stack values, and CPU percentage use explicit u32 stores.
- `name` is exactly `BRUCE_PROCESS_NAME_MAX` bytes and is guaranteed terminated.
- Native `size_t` values must fit `UINT32_MAX` before any output is committed.
- Boolean fields are written as `0` or `1` bytes.
- Padding bytes are zeroed.

If a native size does not fit wasm32, return `BRUCE_ERR_RESOURCE_LIMIT` and
leave the guest output unchanged.

### Dialog Choices

On wasm32, each `bruce_dialog_choice_t` contains four 32-bit guest pointers.
The host wrapper must:

1. Validate `choice_count` against a private limit before multiplication.
2. Validate the complete guest choice array.
3. Read each nested pointer as little-endian u32.
4. Require valid `label` and `value` strings.
5. Permit zero for optional `icon_name` and `right_text`, validating nonzero
   values as strings.
6. Build a bounded temporary native `bruce_dialog_choice_t` array.
7. Validate the guest `out_selected` u32 before opening the dialog.
8. Store the selected index only when Core returns `BRUCE_OK` and the index
   fits u32.
9. Free temporary native storage on every return path.

Choose and document a conservative maximum choice count and total nested text
size. The limit protects firmware memory from a valid but hostile module and
must be large enough for current built-in dialogs. Prefer fixed bounded stack
storage. If dynamic native storage is necessary, use process-tracked
`memory__malloc()` so forced process teardown reclaims it even when KILL occurs
before normal wrapper cleanup; do not use an untracked stack-local `calloc()`
allocation for a blocking dialog.

## Exact Clock App Dependency Slice

The following list is the implementation checklist. The host import name must
remain the public function name under the existing `bruce_sdk` module.

This slice exposes what `clock_app.c` uses, not every function declared by
`core_sdk/clock.h`. `clock__get_utc`, `clock__set_local`, `clock__sync_ntp`,
`clock__get_sync_status`, and `clock__get_ntp_server` remain follow-up work.

### Already Present, Retain Through Guest Adapter

- `runtime__now`
- `runtime__delay`
- `runtime__gui_requested`
- `process__current_id`

The rest of the existing initial SDK imports must continue to work; adding the
guest adapter must not regress runtime, process control, permissions, raw
stdio, sessions, or `memory__get_stats`.

### Scalar Forwarders

- `config__get_time_clock24hr`
- `config__get_theme_primary`
- `config__get_theme_secondary`
- `config__get_theme_background`
- `display__width`
- `display__height`
- `display__begin_frame`
- `display__fill_screen`
- `display__draw_rect`
- `display__set_text_bg_color`
- `display__set_text_color`
- `display__set_text_size`
- `display__present`
- `input__flush`

Use exact WAMR signatures based on wasm32 C lowering. Narrow C integer
parameters still arrive in an `i32`; explicitly cast to the public Core type.

### Validated String Calls

- `display__draw_centre_string`
- `dialog__message`

Validate every required string before calling Core. A translated string is
borrowed only for the duration of the call.

### Validated Output Calls

- `clock__get_local`
- `process__snapshot`
- `input__wait`

For `input__wait`, validate a four-byte output location and use a native
`int32_t` temporary. Store it only when Core returns `BRUCE_OK`.

### Complex Dialog Calls

- `dialog__choice`
- `dialog__number_input`

For `dialog__number_input`, validate required/optional input strings according
to `core_sdk/dialog.h`, require a nonzero buffer size, validate the complete
writable range, and ensure the native call cannot observe an unterminated guest
string where it expects one. Core writes directly only after the entire range
has been validated. If implementation inspection shows Core retains any input
pointer, copy that input to temporary native storage instead.

### Guest-Only Calls

- All `ap_*` functions through the existing parser source
- `memory__malloc`, `memory__calloc`, `memory__realloc`, `memory__free`
- `stdio__printf`, `stdio__vprintf`
- `snprintf`, `vsnprintf`, `sscanf`, `strcmp`, `strlen`, `memcpy`, `memset`,
  `memcmp`, `strchr`, `strtol`, `strtod`, and other libc dependencies pulled by
  Clock or the parser

These names must not appear in the module's host import section.

## Wrapper-Specific Semantics

### Output Commitment

Follow the native API's documented output behavior. Unless the Core API says
otherwise, write guest output only after a successful Core call. This avoids
partially changing guest structures on failure.

### Blocking Calls

Dialogs and input waits may block while the guest is suspended in an import.
Validated plain output buffers remain valid because guest code cannot grow or
mutate its memory during that call. Pointer-containing inputs should still be
translated into a temporary native top-level structure, and copied fully if
Core could retain or asynchronously use any nested pointer.

### Permissions

Do not duplicate permission policy in the WASM binding. Imports execute on the
WASM application's Core process task, so existing Core functions continue to
resolve the correct process and enforce their normal permissions.

### Exceptions And Invalid Calls

Malformed app pointers should return a Bruce error, clear the exception raised
by WAMR's validator, and not terminate the process. A WAMR/runtime failure
outside normal argument validation may still terminate execution through the
existing loader path.

## Registration

Add each host wrapper to the existing `s_native_symbols` array in
`wasm_bruce_host_adapter.c`. Keep the existing comment and policy that pointer
parameters use `i32` WAMR signatures and are manually validated.

Do not register:

- `ap_*`
- guest allocator functions
- variadic stdio functions
- libc functions

WAMR sorts and retains the symbol array, so it must remain writable static
storage for the runtime lifetime.

## Build And Import Validation

The final Clock module must import only supported symbols from `bruce_sdk`, with
the exact WAMR function types registered by the host.
At minimum, the build-time checker must catch:

- an accidental `env.*` import caused by a missing guest adapter declaration;
- any `wasi_snapshot_preview1.*` import;
- an unresolved libc function;
- a Core SDK function that has no registered WASM binding;
- an imported memory, table, or runtime helper not allowed by loader policy.

Keep the existing one-memory, four-page maximum and exported `main(int, char
**)` contract. Measure the linked Clock module and linear-memory requirements;
do not raise firmware loader limits merely to avoid dead-code elimination or
libc configuration work.

## Test Plan

### Host Binding Tests

Add focused coverage for the validation/serialization logic. If direct WAMR
instance tests are impractical in the existing selftest framework, factor only
the byte codecs into pure private helpers and test those, then cover real
address validation with executable WASM fixtures.

Add named selftest files such as
`src/modules/selftest/wasm_bruce_sdk_test.c/.h`, list the source in
`BRUCE_SELFTEST_SOURCES` in `src/CMakeLists.txt`, and invoke each test from
`src/modules/selftest/selftest.c`. If an executable WASM fixture is embedded,
also add it to `EMBED_FILES` and document how it is regenerated.

Required negative cases:

- zero required pointer;
- pointer at the end of memory with a nonzero size;
- offset-plus-size overflow;
- array count multiplication overflow;
- unterminated required string;
- invalid optional nonzero string;
- dialog choice array with one invalid nested string;
- excessive dialog choice count/text total;
- output structure crossing the memory boundary;
- synthetic value too large for wasm32 serialization in a host-side pure codec
  test. ESP firmware has 32-bit `size_t`, so this case cannot arise naturally
  from a process snapshot on the target.

Required behavior cases:

- valid date/time field encoding;
- valid process snapshot field encoding and zeroed padding;
- selected dialog index written only on success;
- input code written only on success;
- existing stdio read/write size outputs remain little-endian u32.

### Guest Tests

Build a small noninteractive WASM fixture through the same build support used
by applications. It should exercise:

- explicit `bruce_sdk` imports;
- `memory__malloc/calloc/realloc/free` in linear memory;
- `snprintf`, `sscanf`, and string comparison;
- `stdio__printf` formatting followed by imported raw output;
- parser creation, commands, options, positional arguments, help, and cleanup;
- one scalar import and one validated output import.

The fixture's import section must pass the same allowlist check as a real app.

Factor the Python import parser/checker into importable functions and add
host-side unit tests for malformed/truncated LEB128, invalid section envelopes,
unexpected modules, WASI and `env` imports, non-function imports, invalid type
indices, duplicate imports, and signature mismatches. These tests are separate
from firmware selftests and must run without ESP-IDF.

### Clock Build Acceptance

Clock's manifest initially lists only `elf`, and `build_modules.py` rejects an
explicit target not declared by the module. Add `"wasm"` to Clock's build
targets as part of the implementation immediately before build acceptance,
then run:

```sh
python3 native_apps/tools/build_modules.py --target wasm --module clock
```

It must compile the unchanged `src/modules/clock/clock_app.c`, append its
manifest, pass import validation, and produce
`native_apps/build_modules/clock.wasm` within existing loader size/memory
limits.

Do not add Clock to the manifest's WASM targets earlier in the implementation,
because normal all-module discovery would otherwise select an artifact whose
bindings and guest runtime are still incomplete.

### Firmware Acceptance

Run the normal firmware build after adding host wrappers:

```sh
source ~/esp/idf/export.sh
idf.py build
```

Also keep the SDK-only WASM loader compile check in `src/CMakeLists.txt`
passing. If host binding source is split, add every new firmware-side source to
both the component `SRCS` list and `bruce_sdk_builtin_wasm_loader_check`.

On hardware or the supported integration environment, verify:

1. `clock show` behavior from the WASM artifact matches the built-in/ELF
   command for 12-hour and 24-hour configuration.
2. `clock timer HH:MM:SS` parses valid input and rejects malformed input.
3. GUI Clock draws, opens the choice dialog, accepts number input, and reacts
   to Back/Select.
4. Background/foreground handoff still follows `process__snapshot` and
   `input__wait` behavior.
5. Terminating the process during delay/input/dialog work does not leak native
   temporary allocations or leave WAMR state active.
6. Existing initial WASM runtime/process/permission/stdio/memory imports still
   execute successfully.
7. Force-KILL a fixture while `dialog__choice()` is blocked and verify
   process-tracked temporary memory returns to its baseline.

## Documentation Updates During Implementation

Update `src/modules/loaders/wasm/README.md` when the implementation lands:

- Replace the "initial SDK slice" list with the actual supported import list.
- Explain that application source uses normal `core_sdk` headers while the
  build links the private guest adapter.
- Document that libc and the argument parser execute in guest memory.
- Document which Core APIs remain unavailable because they return/retain native
  pointers or require callbacks.
- Document the WASM compiler/sysroot requirement and import allowlist.

Update the WASM portion of `ARCHITECTURE.md` to state that the loader binding is
the pointer-validation boundary. Do not duplicate all individual imports in
the architecture contract; the definitive list remains
`wasm_bruce_host_adapter.c` as documented today.

## Implementation Order

1. Add private wasm32 layout constants and reusable host validation helpers.
2. Add `wasm_bruce_guest_adapter.c` for all existing imports and wire it into both WASM
   build scripts.
3. Add/configure freestanding wasm32 libc and reject all non-Bruce imports.
4. Implement guest allocator and formatted stdio.
5. Compile the complete existing argument parser into WASM.
6. Add scalar Clock wrappers and registrations.
7. Add date/time, process snapshot, and input-wait output wrappers.
8. Add validated display/message string wrappers.
9. Add dialog choice and number-input wrappers with bounded temporary storage.
10. Build and execute the guest fixture, including malformed-pointer cases.
11. Enable the Clock manifest's `wasm` target.
12. Build and run Clock unchanged as WASM.
13. Update WASM README and architecture documentation.
14. Run firmware build and regression acceptance.

## Definition Of Done

- No file under `src/core_sdk/` changed.
- Built-in and ELF Clock source/build behavior is unchanged.
- The same `clock_app.c` builds for WASM without WASM conditionals.
- The artifact imports only allowlisted names from `bruce_sdk`.
- No libc, WASI, allocator, variadic, or `ap_*` name is a host import.
- Every host API pointer is validated before translation or serialization.
- No native pointer is returned to or retained from the guest.
- Invalid guest pointers return errors without crashing firmware.
- Clock CLI and GUI behavior works from the WASM artifact.
- Existing WASM imports retain their current behavior.
- Firmware, loader compile checks, guest tests, and Clock build acceptance pass.

## NES Follow-Up Slice

After Clock, use `native_apps/examples/nes` as the next compatibility and
performance target. NES must remain a separate slice: Clock's bindings do not
cover its storage, framebuffer, audio, retained timer pointer, build inputs, or
linear-memory requirements.

The NES goal is again to compile the existing application and Nofrendo sources
without adding WASM conditionals to them. Do not change `src/core_sdk/` for this
slice.

### Additional Core Bindings

Add these imports after the Clock slice is stable:

- Storage: `storage__open`, `storage__read`, `storage__write`,
  `storage__seek`, and `storage__close`.
- Dialog: `dialog__pick_file`.
- Display: `display__game_mode`, `display__color565`,
  `display__fill_rect`, and `display__draw_rgb_bitmap`.
- Input: `input__read`; the public inline `input__poll` then works unchanged.
- Audio: `audio__stream_sample_rate`, `audio__stream_open`,
  `audio__stream_write`, and `audio__stream_close`.
- Runtime timers: `runtime__timer_start`, `runtime__timer_wait`, and
  `runtime__timer_stop`, using the bridge described below rather than retaining
  a guest pointer in Core.

NES also reuses Clock-slice functions including display dimensions/frame
begin/present/fill, runtime now/delay, and guest-local allocation/libc.

### Buffer And Structure Rules

- Storage paths are required validated guest strings. File IDs remain numeric
  Core handles.
- Storage read/write validate the complete guest byte span and a wasm32
  `size_t` output. Check `size * count` overflow in the guest stdio facade
  before calling storage.
- `storage__seek` accepts the wasm32-lowered signed 64-bit offset and writes an
  explicit little-endian u64 position only on success.
- `dialog__pick_file` validates both optional input strings according to the
  public contract and the complete output path buffer before calling Core.
- `input__read` uses a native `bruce_input_event_t` temporary and serializes
  every field into an explicitly asserted wasm32 layout. Do not copy native
  padding.
- `display__draw_rgb_bitmap` rejects negative dimensions and multiplication
  overflow, validates exactly `width * height * sizeof(uint16_t)` guest bytes,
  and borrows the translated buffer only for the synchronous Core call.
- `audio__stream_write` validates
  `frame_count * channels * sizeof(int16_t)` bytes. The binding must remember
  the channel count accepted by `audio__stream_open` per WASM instance and
  clear it on close or process cleanup.

Use existing Core process ownership and permissions for files, game mode, and
audio. No permission policy belongs in the WASM wrapper.

### Retained Timer Pointer

`runtime__timer_start()` is not a normal output-buffer wrapper. Core retains
the supplied counter pointer and updates it asynchronously. It must never
retain a translated pointer into guest linear memory.

Add bounded timer bridge slots to `wasm_loader_process_ctx_t` and attach that
context to the WAMR instance with its custom-data facility. Each slot contains:

```text
Core timer ID
native aligned volatile u32 counter
guest counter offset
active flag
```

The WASM `runtime__timer_start` wrapper must:

1. Validate and require alignment of the guest u32 counter and timer-ID output.
2. Reserve a bounded bridge slot owned by this module instance.
3. Initialize the native counter from the guest counter.
4. Call the unchanged Core `runtime__timer_start()` with the native bridge
   counter.
5. Save the returned Core timer ID and guest offset.
6. Commit the timer ID to guest memory only after success.

The `runtime__timer_wait` wrapper calls Core with the numeric timer ID and,
after a successful tick, validates the saved guest offset again and copies the
native counter back to guest memory. This is sufficient for Nofrendo's loop:
it waits when no tick has appeared and reads the counter after the wait returns.

The `runtime__timer_stop` wrapper stops the Core timer before releasing the
bridge slot. Loader teardown must stop all active bridge timers before
deinstantiating WAMR or freeing the process context. Confirm the process
teardown order for TERM and forced KILL so Core can never update a bridge after
its context is freed.

Document that this bridge synchronizes the guest counter at SDK call
boundaries. Do not advertise it as general asynchronous guest-memory mutation
until WAMR provides a proven pinning/lifetime mechanism.

### External Memory

NES currently attempts to move read-only ROM/VROM buffers through
`memory__external_alloc`, `memory__external_write`, and
`memory__external_map`. A native PSRAM/swap mapping cannot become a directly
dereferenceable WASM pointer.

For the first NES WASM build, define guest adapter implementations of
`memory__external_alloc/write/map/free` that return
`BRUCE_ERR_UNSUPPORTED` without host imports. The existing
`mem_commit_readonly()` path then keeps its original guest allocation and
continues correctly. Do not return a native mapped pointer or silently copy a
mapping while claiming zero-copy semantics.

This fallback means the full ROM and VROM remain in linear memory. Treat the
resulting memory requirement as an explicit feasibility gate, not a reason to
remove validation or bypass WAMR limits.

### Nofrendo Build Inputs

The current WASM app build only discovers sources below
`native_apps/examples/nes/main`. The native CMake build additionally compiles
the sources listed by `components/nofrendo/nofrendo_sources.cmake`, excluding
the desktop `config.c`.

Extend external-app WASM metadata/build handling so NES can declare:

- `main.c`, `nes_osd.c`, `nes_video.c`, `nes_input.c`, `nes_sound.c`, and
  `bruce_stdio.c`;
- the same Nofrendo source groups used by `nofrendo_sources.cmake`;
- include directories for the NES frontend, Nofrendo, the external SDK, and
  `src/`;
- the warning policy currently required by the native integration;
- `-O2`, matching the native emulator's performance build.

Prefer adding a declarative `build` object to the NES manifest and teaching
`build_apps.py` to consume it, using the module build schema where practical.
Do not duplicate a long hard-coded NES source list inside the Python tool.

Ensure every Nofrendo function pointer and callback remains guest-local. None
of those callbacks should cross into Core merely because the source is being
built for WASM.

### Memory Feasibility Gate

The loader currently permits at most four 64 KiB pages (256 KiB). NES needs
linear memory for mutable emulator state, ROM/VROM fallback buffers, video
scratch rows, libc state, stack, and heap. Many ROMs alone approach or exceed
the current limit.

Before changing the global limit:

1. Build Nofrendo with section garbage collection and record module file size,
   static data size, minimum initial pages, and peak pages.
2. Test a small mapper-0 ROM first and record ROM/VROM allocation sizes.
3. Determine where WAMR linear memory is allocated on ESP-IDF and whether a
   larger region can safely use PSRAM on supported boards.
4. Measure impact on boards without PSRAM.
5. Keep a declared maximum in every module and retain a firmware-side hard cap.
6. If limits become board-dependent, reject an oversized module before
   instantiation with `BRUCE_ERR_RESOURCE_LIMIT` and a clear loader message.

Do not raise the four-page cap globally until these measurements identify a
safe allocator and board policy. Supporting only ROMs that fit a documented
limit is acceptable for the first experiment.

### Performance Gate

The native Nofrendo integration already uses `-O2` because CPU and PPU
emulation are frame-budget constrained. WAMR interpretation adds overhead, so
a successful build is not sufficient to mark NES supported.

Measure separately:

- CPU and PPU emulation time;
- RGB565 conversion/scaling time;
- host `display__draw_rgb_bitmap` submission time;
- frame present time;
- audio synthesis and `audio__stream_write` time;
- achieved frame rate and dropped frames.

Use the existing NES performance reporting where possible. Start with a small
ROM and the normal audio configuration so the unchanged source path is tested.
If interpreted WASM cannot approach usable speed, evaluate WAMR AOT as a
separate loader-format/security project; do not weaken pointer validation or
rewrite Core APIs to hide interpreter cost.

### NES Acceptance

NES is complete only when:

- the existing NES frontend and Nofrendo sources build without WASM-specific
  source branches;
- the artifact imports only exact allowlisted `bruce_sdk` function signatures;
- a ROM can be selected or passed on the command line and read through storage
  bindings;
- input press/release events operate the controller and Back exits;
- RGB frames render through the validated bitmap binding;
- timer bridge cleanup is safe under normal exit, TERM, and KILL;
- audio opens, writes, and closes without retaining guest pointers;
- unsupported external mapping falls back to guest memory correctly;
- declared/peak linear memory stays within the adopted board policy;
- measured performance is documented, including an explicit statement if the
  build is functional but not playable.

## Follow-Up Slices

After Clock is complete, expose further Core SDK capabilities one vertical
slice at a time. Classify each API before implementation:

- scalar forwarding;
- validated input/output buffer;
- fixed structure serialization;
- deep translation of pointer-containing structures;
- guest-owned copy of a borrowed native result;
- callback trampoline required;
- incompatible with WASM memory semantics.

Do not mechanically mirror `elf_loader_sdk_symbols.c`. Each new WASM pointer or
callback shape requires an explicit binding design and tests.
