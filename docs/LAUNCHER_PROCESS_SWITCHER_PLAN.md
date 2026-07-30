# Launcher Process Switcher Plan

## Purpose

Add a launcher option that shows up to four live GUI processes at once and lets the
user switch to one. More than four processes are displayed in pages of four.

The switcher uses the compositor defined in
`docs/COMPOSITOR_IMPLEMENTATION_PLAN.md`. It does not allocate private process
framebuffers and does not scale screenshots. Applications reflow into their
assigned tile dimensions.

## Current Launcher

The current GUI launcher:

- Draws a fullscreen border and status line at
  `src/modules/bruce_launcher/bruce_launcher_app.c:152-172`.
- Draws menu options and flushes at
  `src/modules/bruce_launcher/bruce_launcher_app.c:174-249`.
- Owns the GUI input loop at
  `src/modules/bruce_launcher/bruce_launcher_app.c:268-323`.
- Launches selected GUI applications in the foreground at
  `src/modules/bruce_launcher/bruce_launcher_app.c:251-258`.
- Calls `display__fill_screen()` while drawing its main border, which would
  erase live process tiles if reused unchanged.

The process registry already provides live snapshots through
`process__list()` and `process__snapshot()` in `src/core_sdk/process.h:31-54`.

## User Experience

Add a launcher menu option named `Processes` or `Process switcher`.

The overview contains:

- A launcher-owned top status area.
- Up to four process tiles.
- A visible selection border outside each tile's content rectangle.
- A process name associated with each tile.
- A page indicator when more than four GUI processes are available.
- Navigation hints appropriate for the current board input controls.

The visual design remains launcher-owned. Core only assigns viewports and clips
application drawing.

## Process Selection

Build the candidate list from live process snapshots.

Include processes that:

- Are live.
- Were launched with GUI context.
- Are not stopping.
- Are not the current launcher process.
- Are appropriate for foreground switching.

Exclude internal Core workers and non-GUI service processes.

Keep a stable ordering while the overview is open. Recommended initial order
is most recently foregrounded first if Core exposes that information; otherwise
use the stable order returned by a single process-list snapshot and preserve IDs
across refreshes.

When processes exit, remove their IDs and clamp the current page and selection.

## Pagination

Use pages of four:

```text
page_count = (process_count + 3) / 4
page_start = page_index * 4
page_size = min(4, process_count - page_start)
```

Only current-page process IDs receive tile viewports. Processes on other pages are
hidden and their display calls become no-ops.

Changing pages must:

1. Clear the old tile assignment transactionally.
2. Compute the new tile content rectangles.
3. Clear newly assigned regions.
4. Install the new tile assignment.
5. Draw launcher chrome and labels.
6. Allow applications to redraw during later loop iterations.

No display resize events are sent.

## Layout

The launcher computes tile rectangles and passes them to
`display__set_tiles()`.

The top status bar is launcher-owned and is not globally reserved by Core.
Tile content starts below that bar while the overview is open.

Leave gutters around tile rectangles. Selection borders and process labels must be
drawn in those gutters so process clipping prevents applications from overwriting
launcher chrome.

Use deterministic splits and give odd remainder pixels to the final row or
column.

Recommended layouts:

- One process: one large content rectangle.
- Two processes: split along the longer available axis.
- Three processes: a 2x2 grid with one launcher-owned empty cell.
- Four processes: a 2x2 grid.

The exact geometry should be calculated from `display__width()` and
`display__height()` so portrait and landscape boards both work.

## Rendering Ownership

The launcher remains the effective foreground process throughout the overview and
therefore receives all physical input.

Current-page processes remain background processes but receive compositor tile
assignments. They may render and present, but `input__read()` returns
`BRUCE_ERR_NOT_FOREGROUND`.

The input handoff fix is required before this feature. A process blocked in input
when it loses foreground must be woken and revoked. That return allows the app
to continue its loop, observe tile dimensions, and perform its first reflowed
draw.

Applications must avoid spinning when background input is unavailable. Their
loop should use `runtime__delay()` or another bounded wait.

## Launcher Frames

Split launcher drawing into:

- Full initial screen/layout drawing.
- Incremental selection-border drawing.
- Incremental page drawing.
- Incremental status-icon drawing.

Do not call the current `bruce_launcher__draw_main_border()` unchanged while
tiles are active because it starts with `display__fill_screen()`.

Every launcher redraw must use:

```c
display__begin_frame();
/* draw only the intended launcher-owned regions */
display__present();
```

Because the launcher context overlaps the dashboard, its frame may need a
privileged full-layout lease that waits for all affected tile leases. Keep such
frames brief and avoid redrawing process content.

## Status Icons

The launcher lists global icons through `status_icon__list()` and renders them
in its top status bar.

Use the returned revision to avoid unnecessary redraws. Copy the icon snapshot,
release the registry lock, then draw.

The launcher chooses icon colors and truncation policy. Core does not draw the
bar and does not reserve it for fullscreen applications.

See `docs/NOTIFICATIONS_AND_STATUS_ICONS_PLAN.md` for registry semantics.

## Input Mapping

Use existing normalized input codes and adapt navigation to available board
controls.

Required actions:

- Move selection among visible tiles.
- Move to the previous or next page.
- Select the highlighted process.
- Exit back to the normal launcher menu.
- Optionally stop or kill a selected process only through a separate explicit
  action and confirmation.

Do not forward raw launcher input to tiled processes.

## Selecting a Process

Selecting a tile must:

1. Capture the selected process ID.
2. Clear all tile assignments.
3. Hide the other preview processes.
4. Foreground the selected process through `process__foreground()`.
5. Give it the fullscreen compositor viewport.
6. Relinquish launcher input ownership through the normal foreground stack.
7. Let the selected application's loop observe fullscreen dimensions and
   redraw.

Do not copy or scale the tile into a fullscreen buffer. The process performs a
normal reflowed redraw.

When that process backgrounds or exits, the foreground stack restores the
launcher. The launcher then redraws its current menu or process-overview state.

## Empty and Stale States

If no eligible GUI processes exist, display a nonmodal empty state and keep back
navigation active.

Before foregrounding or controlling a selected process, call `process__snapshot()`.
If it has exited, remove it and redraw instead of treating it as a fatal error.

If `display__set_tiles()` returns `BRUCE_ERR_BUSY` because an affected process has
an active frame, retain the old layout and retry in a later launcher loop
iteration. Do not force-release another process's frame.

## AppRunner Capacity

`src/core/app_runner/app_runner.c` defines `APP_RUNNER_MAX_APPS` as 16 and
registers seven default commands, leaving capacity for selftest and future
built-in commands.

If the process switcher is a separate built-in command, the registry has capacity,
but a launcher mode remains the smaller design and avoids a second shell process.

## Terminal Behavior

The launcher process switcher is primarily GUI. The terminal may continue using
existing `process__list()` and process control APIs. Do not put process-switcher-specific
dispatch into the generic terminal parser.

If a terminal command is desired, implement a thin built-in process command over
the same Core process APIs.

## Tests

Add launcher and compositor tests covering:

- Empty process list.
- One through four process layouts.
- More than four processes and page navigation.
- Process exit on the current page.
- Process exit on another page.
- Selection clamping after removal.
- Stable process ordering while refreshing snapshots.
- Launcher remaining foreground during preview.
- Tiled processes drawing but not receiving input.
- Hidden processes producing no framebuffer changes.
- Tile border and status regions protected from process drawing.
- Portrait and landscape layouts.
- `BRUCE_ERR_BUSY` retry during active tile frames.
- Selecting a tile and foregrounding its process.
- Launcher restoration after selected process exit/background.
- Status-icon revision redraw.
- No full-screen clear during incremental dashboard updates.

Relevant existing tests include:

- `src/modules/selftest/launcher_test.c`.
- `src/modules/selftest/process_test.c`.
- `src/modules/selftest/input_test.c`.

Add new selftest sources to `src/CMakeLists.txt` and invoke them explicitly from
`src/modules/selftest/selftest.c`.

## Files Expected to Change

- `src/modules/bruce_launcher/bruce_launcher_app.c`
- `src/core_sdk/display.h`
- `src/core/display/display.c`
- `src/core/process/process.c`
- `src/core/input/input.c`
- `src/modules/selftest/launcher_test.c`
- `src/modules/selftest/process_test.c`
- `src/modules/selftest/input_test.c`
- `src/CMakeLists.txt`
- `ARCHITECTURE.md`
- `migration_plan.md`

## Acceptance Criteria

- The launcher displays live GUI processes in pages of at most four.
- Current-page processes receive correct reflowed dimensions.
- Other background processes cannot modify the framebuffer.
- Tiled processes never receive physical input.
- A blocked old foreground cannot consume launcher input.
- Launcher status, labels, gutters, and selection borders cannot be overwritten
  by process drawing.
- Page changes and process exits do not expose mixed layouts.
- Selecting a tile foregrounds that process and gives it fullscreen dimensions.
- No private per-process pixel framebuffer is allocated.
- No display resize event is required.
