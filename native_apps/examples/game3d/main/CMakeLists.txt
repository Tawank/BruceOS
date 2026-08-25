# Jet (the 3D rasteriser this app renders with -- see main.cpp and
# README.md) is declared as an ordinary git-based component dependency in
# idf_component.yml rather than vendored into this repo, so it needs no
# special handling here beyond a normal REQUIRES -- the component manager
# fetches it into this app's gitignored managed_components/ at build time,
# and its own upstream CMakeLists.txt (already idf_component_register()-
# shaped) builds it as component "jet", exposing Jet.hpp etc. on this
# component's include path automatically. Getting its compiled code into
# the final ELF (project_elf() links only "main" by default) is handled in
# the parent CMakeLists.txt via ELF_COMPONENTS, not here.
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "." "../../../include" "../../../../src")

# Jet is C++17; the rest of native_apps/examples is plain C, so this project
# has never needed to set a C++ standard before now.
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Required, not stylistic: project_elf() builds with -nostdlib (see
# components/elf_loader/elf_loader.cmake), so this app never links
# libstdc++/libsupc++. The loader's SDK symbol table (see the C++ ABI
# comment block in src/modules/loaders/elf/elf_loader_sdk_symbols.c) only
# covers a minimal freestanding shim -- operator new/delete, a
# __cxa_pure_virtual trap, a no-op __cxa_atexit, and libstdc++'s __throw_*
# helpers -- nothing that real exceptions, RTTI, or thread-safe
# function-local statics need. Building without them isn't an optimisation
# here, it's what makes this link at all. Jet's own CMakeLists.txt doesn't
# set these (it's shared across Jet's other, non-ELF-constrained frontends),
# so they're set again for it specifically below.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti -fno-threadsafe-statics)
idf_component_get_property(jet_lib jet COMPONENT_LIB)
target_compile_options(${jet_lib} PRIVATE
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    # Verified against a real build (ESP-IDF v6.0 / xtensa-esp-elf
    # esp-15.2.0_20251204): Jet's JetConfig-gated code does trip both of
    # these under ESP-IDF's default warning-promotion set (a parameter or
    # local only used when a particular Jet feature is compiled in) --
    # confirmed false positives, not real bugs, given this app's
    # main/JetConfig.hpp. Re-check if JetConfig.hpp ever changes which
    # features are enabled.
    -Wno-error=unused-parameter
    -Wno-error=unused-variable)
