# Agent guidance for BruceIDF

## Working with legacy reference code

`BrucePIO_legacy/` is a read-only behavioral reference. It is not compiled, edited, or deleted.

When asked to make a new function or module **behave like** a legacy one:

- **Do not copy-paste** code from `BrucePIO_legacy/` into the new implementation.
- **Do not** add a new function named `legacy_*` (or similar) that wraps or mimics the old function.
- **Do** implement the desired behavior using the current BruceIDF architecture: public `core_sdk/` APIs, the module naming convention (`module__action()`), and the ESP-IDF/Core abstractions already in place.
- It is fine to mention the old behavior as the visual/UX reference in comments or docs, but the implementation must be native to the new codebase.

## Architecture quick reference

- `src/core/` — private Core implementation; only Core source and `modules/selftest` may include it.
- `src/core_sdk/` — public SDK used by every built-in module and external ELF app.
- `src/modules/` — built-in apps and loader modules; include only `core_sdk/*.h` headers.
- `BrucePIO_legacy/` — reference only. See `migration_BruceIDF.md` for the full architecture contract.

## Build notes

- Activate ESP-IDF with `source ~/esp/idf/export.sh`.
- `idf.py build` / `ninja all` from the repo root (or `build/`).
- Ask before running long-running `idf.py reconfigure` or full builds unless the user has already approved.
