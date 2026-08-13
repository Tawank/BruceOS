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
   stack, a 64 KiB application heap, and at most four 64 KiB linear-memory pages.
   The heap is additional host-managed WAMR instance memory; it is not part of
   the four-page linear-memory policy. Modules must declare one wasm32 memory,
   with initial and declared maximum sizes no greater than four pages.
  The manifest `stackSize` controls the separate Core process stack.
- INT and TERM interrupt WAMR execution and then use normal process teardown.
- The restricted Bruce ABI requires an exported `int main(int argc, char **argv)`
  entry. `_start`, void main, and WASI entry points are not accepted.
- Manifest inspection checks the v1 header, bounded section envelopes, and the
  Bruce custom-section bounds. It does not claim full WebAssembly validation;
  WAMR validates module semantics at launch. Missing, duplicate, oversized, or
  malformed Bruce manifest payloads use filename fallback when envelopes are valid.

## Allocation

Application `malloc`, `calloc`, `realloc`, and `free` must operate in WASM
linear memory. They are supplied by the application's freestanding toolchain,
not imported from Bruce. The ELF mapping to `memory__malloc()` cannot be used:
that function returns a native ESP pointer, which is not addressable by WASM.

`memory__get_stats` is available for system memory diagnostics. It writes eleven
little-endian `uint32_t` fields in the order declared by
`wasm_bruce_memory_stats32_t`. Values too large for wasm32 return
`BRUCE_ERR_RESOURCE_LIMIT`.

## Imports

The initial SDK slice exports:

- runtime: `runtime__now`, `runtime__sleep`, `runtime__delay`,
  `runtime__gui_requested`;
- process: current ID/signal, foreground/background switching, signal,
  terminate, pause, resume, kill, and wait;
- permissions: `permission__check` for introspection;
- stdio: raw read/write, line input, and process-owned stdio sessions;
- memory: `memory__get_stats`.

Pointer parameters are wasm32 offsets. Wrappers validate every complete buffer
before calling Core. Native variadic functions, host-pointer allocation APIs,
permission mutation, and APIs that retain caller pointers are not exported.

The exact WAMR signatures are maintained in `wasm_bruce_sdk.c`. A WASM SDK
header/toolchain should declare matching imports using Clang's `import_module`
and `import_name` attributes and format text inside linear memory before calling
`stdio__write`.

## Manifest Section

The standard WASM custom section is named `bruce.manifest`. Its payload is the
same canonical JSON used by ELF and JavaScript manifests. Missing or invalid
manifest metadata falls back to filename metadata only when the WASM binary
itself is structurally valid.
