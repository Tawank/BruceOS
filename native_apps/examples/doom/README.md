# Doom

A native ELF port of [doomgeneric](https://github.com/ozkl/doomgeneric)
(itself built on [Chocolate Doom](https://www.chocolate-doom.org/)). Renders
into a 320x200 RGB565 buffer each frame and scale-blits it with
`display__draw_rgb_bitmap()`; controls below, exit by quitting to the
in-game menu and using its own Quit option (there is no separate
"quit to launcher" binding -- see `main.c`).

## You need your own WAD

No IWAD (`doom1.wad`, `doom.wad`, `freedoom1.wad`, ...) is included in this
repo, for the same reason `nes/`'s README would tell you to bring your own
ROM: it's copyrighted, or at minimum not this project's to redistribute.
Copy one onto the device's storage yourself; launching the app with no
argument opens a file picker rooted at `/` filtered to `.wad` files
(`dialog__pick_file()`, same pattern `nes/main.c` uses for `.nes` ROMs). The
shareware `doom1.wad` and the free [Freedoom](https://freedoom.github.io/)
IWADs both work.

## Controls

| Action | Input |
| --- | --- |
| Move | Up / Down |
| Turn | Left / Right |
| Fire | Button A |
| Use / open door | Button B |
| Strafe | L1 / R1 |
| Automap toggle | Button X |
| Automap zoom out / in | L2 / R2 |
| Menu / back | Back, Menu, or Select (gamepad) |
| Confirm | Select (D-pad), Start, or Button Y |

A physical keyboard, if the device has one, also works via its own ASCII
key events (letters, digits, etc.) -- useful for cheat codes.

## Porting notes

- **Memory.** Doom's own zone allocator (`Z_Init()`/`I_ZoneBase()` in
  `doomgeneric/i_system.c`) wants a single large upfront allocation --
  `main.c` requests exactly 4 MiB via `-mb 4`. `malloc()` inside a Bruce ELF
  app always draws from PSRAM (`memory__malloc()`,
  `src/core/memory/memory.c`), so this is a PSRAM budget, not internal RAM;
  it needs a board with at least that much free. `manifest.json`'s
  `heapSize` is a pre-launch reclaim hint (see
  `elf_loader_app.c`'s `memory__reclaim()` call), not a hard cap -- it's set
  a bit above the `-mb` request to give the reclaim step something to aim
  for.
- **Video.** `doom_video.c` passes `-gfxmode rgb565` to select
  `i_video.c`'s existing 16bpp output path (its default is 32bpp RGBA) --
  that format is already an exact match for `display__draw_rgb_bitmap()`,
  so the vendored renderer needed zero patches for pixel format. The
  platform buffer is pinned to Doom's native 320x200 (rather than the real
  panel's resolution) so `i_video.c`'s own internal scale factor is always
  1:1 and hands back a flat, unscaled frame; `doom_video.c` does the one
  scale-blit to the actual (runtime-queried) screen size itself, batching
  rows per `display__draw_rgb_bitmap()` call the same way `nes_video.c`
  does for the NES's fixed framebuffer.
- **Audio.** Disabled, same as `nes/`'s NES emulation -- the public Bruce
  ELF SDK does not expose an audio stream API. `doomfeatures.h`'s
  `FEATURE_SOUND` is left undefined so the SDL-based sound/music backends
  (`i_sdlsound.c`, `i_sdlmusic.c`) are excluded entirely rather than stubbed;
  `i_sound.c`'s own hooks (`I_InitSound` etc.) already no-op cleanly without
  them.
- **Joystick.** `doomgeneric/i_joystick.c` needed no changes -- like
  `i_sound.c`, this fork already guards its SDL_Joystick calls behind
  `#ifdef ORIGCODE` (never defined here, see `config.h`), so the functions
  `d_main.c` calls unconditionally at startup are already effectively
  no-ops. There's no gamepad axis wiring in this port -- `doom_input.c`
  maps discrete button events only.
- **64-bit multiply.** `m_fixed.c`'s `FixedMul()` -- Doom's single
  hottest-path arithmetic operation -- computes a 64-bit product
  (`(int64_t)a * (int64_t)b`). Xtensa has no 64x64 hardware multiply, so GCC
  lowers this to a `__muldi3` libgcc call; the ELF loader's SDK symbol
  table already exported the analogous 64-bit divide/modulo helpers for the
  same reason (`__divdi3`/`__moddi3`/`__udivdi3`/`__umoddi3`, see
  `elf_loader_sdk_symbols.c`) but not the multiply, so this port added that
  one export alongside them. `FixedDiv()`'s 64-by-32 divide was already
  covered.
- **`zenity` popup skipped, but not because `system()` is a stub.**
  `i_system.c`'s fatal-error path (`I_Error()`) optionally shells out to
  `zenity` for a GUI popup on desktop platforms. `elf_loader_sdk_symbols.c`
  gained a `system()` export for this (`bruce_elf__system()`) -- it's a
  real implementation now, running the given command through BruceOS's own
  shell (`modules/shell/`) as a real child process (added when `vi/`
  needed `:!cmd` -- see `vi/README.md`), not a stub. `main.c` still passes
  `-nogui` regardless, so this path is never attempted here -- there's no
  `zenity` (or any GUI popup tool) on this platform for it to usefully find
  even if it were.
- **No fork.** Unlike `nes/`'s pinned `nofrendo` fork (which carries its own
  ESP-IDF component metadata), the portable engine here is pulled straight
  from upstream [`ozkl/doomgeneric`](https://github.com/ozkl/doomgeneric) at
  a pinned commit -- it isn't itself an ESP-IDF component (no
  `idf_component.yml`/`CMakeLists.txt`), and ESP-IDF's build system
  hard-errors on a `managed_components/*` entry that isn't one, so
  `main/doomgeneric_sources.cmake` fetches it directly at CMake configure
  time (into the build directory, so `idf.py fullclean` cleans it up)
  rather than through `idf_component.yml`. That same file globs its
  sources, excludes the desktop platform frontends (`doomgeneric_sdl.c`,
  `doomgeneric_xlib.c`, `doomgeneric_win.c`, etc. -- this port's frontend is
  `main.c` + `doom_video.c` + `doom_input.c`, following the same
  `DG_Init`/`DG_DrawFrame`/`DG_SleepMs`/`DG_GetTicksMs`/`DG_GetKey`
  interface they implement) and the SDL/Allegro sound and CD-audio backends
  noted above, and idempotently patches `m_misc.c`'s one `errno`-dependent
  line in place (see that file's own comment for why -- an
  `R_XTENSA_TLS_TPOFF` relocation the Bruce ELF loader doesn't support)
  rather than forking upstream just for it.
