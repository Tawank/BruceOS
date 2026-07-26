# Agent guidance for BruceIDF

## Project layout
- `src/` is the only ESP-IDF component (EXTRA_COMPONENT_DIRS src), registered
  in src/CMakeLists.txt with explicit SRCS/REQUIRES/INCLUDE_DIRS lists (add
  new source files there manually).
- `src/idf_component.yml` declares managed component deps (idf-component-manager).
  Added `espressif/cjson` here for JSON parsing (component target name: `cjson`,
  header: `cJSON.h`, functions like cJSON_Parse/cJSON_Print/cJSON_AddXToObject).
  The internal filesystem uses `joltwallet/littlefs` (component target name:
  `littlefs`, header: `esp_littlefs.h`).
- `BrucePIO_legacy/` is the old PlatformIO/Arduino codebase being migrated from -
  useful as a reference for porting logic (e.g. BrucePIO_legacy/src/core/config.cpp)
  but not part of the ESP-IDF build.
- Core code lives in src/core/{app_runner,config,dialog,display,http,input,manifest,memory,permission,stdio,storage,task,wifi}; apps in
  src/modules/*. See migration_plan.md at repo root for the architecture
  (core must stay minimal: HAL + runtime + BruceConfig + AppRunner only).
- Naming convention: `module__action()` for public C API, snake_case fields.
- Public fallible APIs don't have to return `bruce_result_t`; simpler types
  (bool/int/pointer) are fine when documented in the `core_sdk/` header (e.g.
  wifi__is_connected() -> bool, wifi__scan() -> int count/negative BRUCE_*,
  wifi__get_ssid()/get_ip()/get_mac() -> char* or NULL).
- Core-private `*_common.h` headers (e.g. src/core/wifi/wifi_common.h) must
  NOT redeclare the public struct/functions already in the matching
  src/core_sdk/<module>.h - the .c file includes the core_sdk header directly
  for those declarations instead, to avoid conflicting duplicate
  redeclarations when the public signature changes. Private headers should
  only hold genuinely Core-internal-only declarations (can be empty/placeholder).
- Private Core headers are named plain `<module>.h` inside `core/<module>/`
  (e.g. core/app_runner/app_runner.h, core/config/config.h, core/task/task.h),
  not `<module>_common.h`. `core/wifi/wifi_common.h` is the private Wi-Fi header;
  it holds only genuinely internal declarations (`wifi__init()`) and does not
  redeclare the public `wifi__*` API, which lives in `core_sdk/wifi.h`.

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

## Architecture contract

In the ARCHITECTURE.md file, the "BruceIDF architecture contract" section describes the
expected behavior of the Core and built-in modules, and the public APIs that
external apps can rely on. The contract is a living document that should be updated
as new features are added, and it is the authoritative source for what the
Core and built-in modules are expected to do.
For now the BruceIDF is in the Alpha stage, so the contract is not yet complete.
And can be a subject of discussion and change.

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
- `BrucePIO_legacy/` — reference only. See `migration_plan.md` for the full architecture contract.

## Build notes

- Activate ESP-IDF with `source ~/esp/idf/export.sh`.
- `idf.py build` from the repo root (use tail to truncate the output never use it without it). 
