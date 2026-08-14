# Bruce WebAssembly SDK

WebAssembly applications run as external Core processes under WAMR. They import
the supported API from the module named `bruce_sdk`; unknown imports make module
loading fail.

## Security Model

- The permission identity is the filename including `.wasm`, without its path.
- A `bruce.manifest` custom section may contain the canonical Bruce manifest
  JSON. Declared permissions are preflighted before the process starts.
- Protected Core functions retain their normal `permission__check()` behavior
  because imports execute on the WASM application's Core process task.
- WAMR WASI, built-in host libc, pthreads, shared memory, and multi-module
  loading are disabled. Raw host files, sockets, and threads are unavailable.
- Module files are limited to 1 MiB. Instances use an 8 KiB WAMR execution
   stack, an 8 KiB guest shadow stack, a 16 KiB application heap, and at most
   four declared 64 KiB linear-memory pages. WAMR appends the heap to compacted
   fixed linear memory; the build exports `__data_end` and `__heap_base` so
   unused declared memory is not allocated. Modules must declare one wasm32
   memory, with initial and declared maximum sizes no greater than four pages.
   The classic interpreter avoids the fast interpreter's additional
   precompiled-bytecode memory on constrained ESP32 targets.
  The manifest `stackSize` controls the separate Core process stack.
- INT and TERM interrupt WAMR execution and then use normal process teardown.
- The restricted Bruce ABI requires an exported `int main(int argc, char **argv)`
  entry. `_start`, void main, and WASI entry points are not accepted.
- Manifest inspection checks the v1 header, bounded section envelopes, and the
  Bruce custom-section bounds. It does not claim full WebAssembly validation;
  WAMR validates module semantics at launch. Missing, duplicate, oversized, or
  malformed Bruce manifest payloads use filename fallback when envelopes are valid.

## Allocation

Application `malloc`, `calloc`, `realloc`, and `free` operate in WASM linear
memory through `bruce_sdk.memory__malloc/calloc/realloc/free`. Their host
wrappers use WAMR's module allocator and return guest offsets, never native ESP
pointers. The module heap belongs to the process-owned WAMR instance and is
reclaimed when that instance is deinstantiated during loader cleanup. Shared
WAMR runtime storage remains on WAMR's global allocator.

`memory__get_stats` is available for system memory diagnostics. It writes eleven
little-endian `uint32_t` fields in the order declared by
`wasm_bruce_memory_stats32_t`. Values too large for wasm32 return
`BRUCE_ERR_RESOURCE_LIMIT`.

## Imports

The supported SDK slice exports:

- runtime: `runtime__now`, `runtime__sleep`, `runtime__delay`,
  `runtime__gui_requested`;
- process: current ID/signal, foreground/background switching, signal,
  terminate, pause, resume, kill, and wait;
- permissions: `permission__check` for introspection;
- stdio: raw read/write, line input, and process-owned stdio sessions;
- memory: `memory__get_stats`.
- Clock: local time reads;
- Config: Clock's 12/24-hour and theme color getters;
- Display: dimensions, frame begin/present, fill, rectangle, text state, and
  centered strings;
- Input: flush and wait-for-press;
- Dialog: message, bounded choice, and number input.

Application source includes the normal `core_sdk` headers. The WASM build links
the private guest adapter and freestanding C runtime under `native_apps/wasm/`,
plus the existing argument parser. Individual guest allocations share WAMR's
process-owned module heap and are reclaimed together at instance teardown.

Pointer parameters are wasm32 offsets. Wrappers validate every complete buffer
before calling Core. Native variadic functions, host-pointer allocation APIs,
permission mutation, and APIs that retain caller pointers are not exported.

The exact WAMR signatures are maintained in `wasm_bruce_host_adapter.c`. The
build validates the linked type and import sections before adding the manifest;
only exact function imports from `bruce_sdk` are accepted. APIs that return or
retain native pointers, callbacks, variadic host calls, and external-memory
mapping remain unavailable.

WASM builds require a Clang installation with the `wasm32` target. They target
WebAssembly MVP to match the restricted WAMR feature set, do not require a WASI
sysroot, and may not contain WASI imports.

## Manifest Section

The standard WASM custom section is named `bruce.manifest`. Its payload is the
same canonical JSON used by ELF and JavaScript manifests. Missing or invalid
manifest metadata falls back to filename metadata only when the WASM binary
itself is structurally valid.
