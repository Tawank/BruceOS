# Game3D

A native ELF example spinning a flat-shaded debug cube via
[Jet](https://github.com/CubeCoders/Jet), a small fixed-function 3D
rasteriser. Renders into a full-resolution RGB565 buffer each frame and
blits it with `display__draw_rgb_bitmap()`; exit with the Back button.

Jet isn't vendored into this repo -- it's declared as an ordinary git-based
component dependency in `main/idf_component.yml`, pinned to a specific
commit, and fetched into this app's own (gitignored) `managed_components/`
by the ESP-IDF component manager on build, the same mechanism already used
here for `espressif/elf_loader`. Its upstream `CMakeLists.txt` is already
shaped as a normal ESP-IDF component (`idf_component_register(SRC_DIRS
"src" ...)`), so it builds as its own `libjet.a` -- see
`CMakeLists.txt` for how that gets linked into the final ELF (`project_elf()`
links only `main` by default; this app extends `ELF_COMPONENTS` to also
pull in `jet`).

This is the first C++ Bruce ELF app -- everything else under
`native_apps/examples/` is plain C -- and that comes with real constraints,
not just style differences:

- **AGPL-3.0-or-later.** Jet is dual-licensed; fetching the git repository
  pulls in its open-source AGPL terms (see the `LICENSE` file in the fetched
  source, or upstream directly), not CubeCoders' commercial license. Linking
  it into this app means the combined work is AGPL-3.0-or-later unless built
  against the commercial license instead. Worth knowing before this ships
  anywhere -- AGPL's source-disclosure obligations are broader than a
  permissive license's, and they apply regardless of whether the source is
  vendored into this repo or fetched at build time.

- **No exceptions, no RTTI, no global constructors.** `project_elf()` builds
  ELF apps with `-nostdlib` (see `components/elf_loader/elf_loader.cmake`),
  so a C++ app never links libstdc++/libsupc++. Making this app link at all
  required adding a minimal C++ ABI shim to the loader's SDK symbol table
  (`src/modules/loaders/elf/elf_loader_sdk_symbols.c`): freestanding global
  operator new/delete (including the nothrow overload), a `__cxa_pure_virtual`
  trap, a no-op `__cxa_atexit` (see below), and the libstdc++ `__throw_*`
  helpers `<vector>`/`<new>` call on allocation failure or an oversized
  request. There's deliberately no exception unwinding, no RTTI
  (`dynamic_cast`/`typeid`), no `__cxa_guard_*` for thread-safe function-local
  statics, and the loader never walks `.init_array` -- a namespace-scope C++
  object with a non-trivial constructor would silently never run it. Both
  `main.cpp` and Jet itself are built with `-fno-exceptions -fno-rtti
  -fno-threadsafe-statics` (see `main/CMakeLists.txt`, which sets this for
  Jet's component explicitly since Jet's own `CMakeLists.txt` doesn't -- it's
  shared across Jet's other, non-ELF-constrained frontends) to match what's
  actually supported, and this app's own code sticks to objects that are
  either function-local/heap-allocated (constructed by an ordinary function
  call, no special runtime support needed) or plain-old-data at namespace
  scope (zero-initialized BSS, no constructor to skip). Any future edits to
  `main.cpp`, or a `JetConfig.hpp` change that pulls in more of Jet, should
  keep to that same rule.

  One exception needed its own fix: `Scene.cpp` has function-local `static
  std::vector<...>`s, and even `-fno-threadsafe-statics` (which only
  suppresses the *initialization* guard) doesn't stop the compiler from
  registering their destructors via `__cxa_atexit` -- harmless here since
  ELF apps have no `exit()`-based teardown path for it to ever run, so the
  loader's stub just reports success without recording anything. Its
  companion `__dso_handle`, though, has hidden ELF visibility by ABI
  convention, which means `project_elf()`'s `-fPIC -shared -fvisibility=hidden`
  link *refuses* to leave it as a runtime-resolved external the way it does
  every symbol in the loader's table -- it must be defined inside the app's
  own linked objects instead, which is why `main.cpp` defines it directly
  (see the comment there). Any other C++ ELF app hitting the same
  function-local-static pattern will need that same line.

  Two of BruceOS's own C headers needed a fix for this app to build at all:
  `core_sdk/input.h` and `core_sdk/runtime.h` were missing the
  `#ifdef __cplusplus extern "C" { ... }` guard that `core_sdk/display.h`
  already had, so a C++ translation unit including them got C++-mangled
  declarations (e.g. `input__check` as `_Z12input__checklb`) that could
  never match the plain C names the loader's SDK symbol table exports --
  the app would build and link but fail to resolve those symbols at load
  time on real hardware. Fixed at the source for both headers. The rest of
  `core_sdk/*.h` has the same latent gap (this is apparently the first C++
  ELF app), but auditing all of them was out of scope here -- fix the ones a
  given C++ app actually includes if this bites again.

- **Build-verified, not hardware-verified.** `python3
  native_apps/tools/build_apps.py --target elf --idf-target esp32s3
  --idf-path "$IDF_PATH" --app game3d` builds clean end to end against a
  real ESP-IDF v6.0 / xtensa-esp-elf (esp-15.2.0_20251204) toolchain,
  including the live fetch of Jet's git dependency: every Jet source file
  and `main.cpp` compile with zero warnings under ESP-IDF's default
  `-Wall -Werror` promotion (plus this app's own narrow `-Wno-error=`
  additions in `main/CMakeLists.txt`, which did turn out to be needed --
  Jet's JetConfig-gated code does trip a few `-Wunused-variable`/
  `-Wunused-parameter` cases), the final `-nostdlib -shared` link succeeds
  (with `jet` pulled in via `ELF_COMPONENTS`, see `CMakeLists.txt`), and
  `build_apps.py`'s `objcopy --add-section .bruce.manifest` step completes,
  producing `native_apps/examples/game3d.elf` (checked into git alongside
  the other examples' prebuilt `.elf` outputs). Every symbol the linked ELF
  leaves undefined was cross-checked against `g_bruce_sdk_elfsyms[]` by hand
  (`objdump -T` against the table's actual entries) and all resolve. What
  that build *doesn't* prove: on-device runtime behaviour (rendering
  correctness, frame pacing, RISC-V targets or other board configs) -- none
  of that was checked, since this sandbox has no target hardware.

  `dependencies.lock` and `sdkconfig`/`sdkconfig.old` here are genuinely
  regenerated output from that verification build (not hand-written or
  copied), matching how every other example in this directory checks in the
  same files.

  One unrelated, pre-existing issue surfaced during this verification: the
  pinned `espressif/elf_loader@^1.3.1` component (`main/idf_component.yml`)
  doesn't compile cleanly against the newlib bundled with the toolchain in
  this sandbox (`__errno`/`__getreent`/`_ctype_` go undeclared in the
  component's own `esp_elf_symbol.c`) -- confirmed present in the stock,
  unmodified `game` example too, so it's a toolchain/component version-skew
  bug, not something introduced here. Whether it affects a real build
  depends on the exact toolchain version in use; if it does, it's unrelated
  to this app or to Jet.

Build like any other native ELF app:

```sh
python3 native_apps/tools/build_apps.py --target elf --idf-target esp32s3 --idf-path "$IDF_PATH" --app game3d
```

This needs network access on first build (or after a cache wipe) to fetch
Jet's git dependency -- everything after that comes from the local
`managed_components/` cache like any other component-manager dependency.

The WASM target isn't available for this app: `native_apps/tools/build_apps.py`
doesn't support compiling C++ sources for WASM yet, and Jet is C++17.
