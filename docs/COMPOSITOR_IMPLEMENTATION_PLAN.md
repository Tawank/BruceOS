# BruceIDF Small Compositor Implementation Plan

> **Status: implemented, then extended.** The single "display state mutex"
> described in "Logical Region Locking" below has since been split into a
> short-lived structural registry lock plus a per-surface (per-process-
> viewport or per-overlay) lock, so unrelated processes/overlays with
> disjoint regions can draw concurrently instead of only being non-blocking
> around the DMA transfer. A generic `display__overlay_*` primitive was also
> added on top of this compositor (see `ARCHITECTURE.md`), and the
> notification banner this document mentions moved off of Core onto it (see
> `docs/NOTIFICATIONS_AND_STATUS_ICONS_PLAN.md`). The rest of this document
> (viewport/tile/frame-lease model, packed partial transfers) is still
> accurate.

## Purpose

This document is an implementation handoff for replacing the current shared,
last-writer-wins display behavior with a small Core compositor.

The agreed design is:

- Keep one shared RGB565 application framebuffer.
- Give the foreground process a fullscreen viewport.
- Make display calls from ordinary hidden processes successful no-ops.
- Let the launcher assign up to four background GUI processes to non-overlapping
  viewports.
- Reflow applications to viewport dimensions; do not scale full-size frames.
- Do not send resize events. Applications discover changed dimensions in their
  normal rendering loop.
- Keep a process's viewport logically locked from `display__begin_frame()` until
  its LCD transfer completes.
- Keep physical input assigned only to the foreground process.
- Let a Core display worker be the only owner of panel transfers.

The input ownership race must be fixed before enabling the process switcher. See
`docs/INPUT_FOREGROUND_HANDOFF_PLAN.md`.

## Current Implementation

The current display is immediate-mode with one global framebuffer and one
recursive mutex:

- `src/core/display/display.c:177-192` contains the panel, mutex, framebuffer,
  dimensions, text state, and cursor state.
- `src/core/display/display.c:264-270` writes pixels directly into the global
  framebuffer and clips only to the physical logical screen.
- `src/core/display/display.c:603-625` allocates one 64,800-byte DMA-capable
  framebuffer (`135 * 240 * 2`).
- `src/core/display/display.c:1118-1128` submits the entire framebuffer in
  `display__flush()`.
- `src/core/display/display.c:339-347` configures an SPI transaction queue depth
  of 10 but does not register a transfer-completion callback.
- `src/core_sdk/display.h:3-17` documents the current immediate-mode behavior.
- `ARCHITECTURE.md:431-432` explicitly says that v1 has no compositor or
  layers; this contract must change with the implementation.

Every primitive currently locks independently. This serializes individual
writes but does not make a complete frame atomic. Text color, background,
cursor, and text size also leak between processes because they are global.

## Required Invariants

The implementation is complete only when these invariants hold:

1. Only the Core display worker submits panel transfers.
2. No process receives a direct framebuffer pointer.
3. Process coordinates are local to the process's current viewport.
4. Every primitive clips to that viewport.
5. Hidden process drawing and presentation return success without changing pixels.
6. Foreground processes render fullscreen unless the caller is the launcher while
   it owns a dashboard layout.
7. Only launcher-assigned background processes may render tiles.
8. Tile assignments cannot change during an active frame or transfer.
9. A tile remains leased through the LCD transfer-completion callback.
10. Killing a process cannot strand a mutex, tile lease, or DMA transfer.
11. Text and cursor state are process-local.
12. Rotation remains a global physical-panel operation.

## Public Frame API

Add to `src/core_sdk/display.h`:

```c
bruce_result_t display__begin_frame(void);
bruce_result_t display__present(void);
```

Retain:

```c
bruce_result_t display__flush(void);
```

Implement `display__flush()` as a compatibility alias for
`display__present()`. Existing foreground applications must continue to work
while call sites migrate to explicit frame boundaries.

### `display__begin_frame()`

`display__begin_frame()` must:

- Resolve the calling Core process before acquiring the display state lock.
- Reject a nested frame with `BRUCE_ERR_INVALID_STATE`.
- Snapshot the caller's viewport and its generation.
- Establish a Core-managed logical lease over the viewport.
- Return `BRUCE_ERR_BUSY` if a conflicting region is already leased.
- Establish a successful no-op frame when the process is hidden.
- Avoid retaining a process-owned FreeRTOS mutex across API calls.

The last point is essential. `process__kill()` can delete an application at any
instruction. Deleting a process that owns a FreeRTOS mutex can strand that mutex
forever. The logical lease must instead be state owned by Core and removable by
the process lifecycle hook.

### `display__present()`

`display__present()` must:

- Validate the caller, frame lease, and viewport generation.
- Return success without panel traffic for a no-op frame.
- Submit a presentation request to the display worker.
- Block the caller until the worker reports transfer completion.
- Keep the logical viewport lease active while queued, packed, submitted, and
  transferred.
- Release the frame and viewport lease on every success and error path.

If the presenting process is force-killed, the display worker must finish or
safely fail the already-submitted transfer, then release the Core-owned lease.

## Process Display Context

Keep one Core context for every live GUI process:

```c
typedef struct {
    bool in_use;
    bruce_process_id_t process_id;

    bool tiled;
    bool hidden;
    bool frame_active;
    bool frame_noop;

    bruce_display_rect_t viewport;
    uint32_t viewport_generation;

    bruce_display_color_t text_color;
    uint32_t text_bg_color;
    bool text_bg_transparent;
    uint8_t text_size;
    int16_t cursor_x;
    int16_t cursor_y;
} display__process_context_t;
```

Defaults must match current behavior: white text, black opaque background, text
size one, and cursor `(0, 0)`.

Move the globals currently at `src/core/display/display.c:186-191` into this
context. Geometry changes do not need to reset text state or cursor position.

Use private process lifecycle hooks declared in `src/core/display/display.h`:

```c
void display__process_created(bruce_process_id_t process_id, bool gui_requested);
void display__process_state_changed(bruce_process_id_t process_id,
                                 bruce_process_state_t state);
void display__process_removed(bruce_process_id_t process_id);
```

Process code may invoke these hooks while holding the process registry lock. Display
code must never call a process API while holding the display state lock.

## Viewport Semantics

The effective foreground GUI process sees the full logical display:

```c
{ .x = 0, .y = 0, .width = screen_width, .height = screen_height }
```

A tiled process sees its launcher-assigned rectangle. A hidden process sees a
zero-sized viewport.

Change `display__width()` and `display__height()` accordingly. The existing
"always positive" documentation in `src/core_sdk/display.h:77-81` must be
removed.

Local coordinates map directly into the viewport without scaling:

```c
screen_x = viewport.x + local_x;
screen_y = viewport.y + local_y;
```

All primitives must clip against both the local viewport and physical display.
`display__fill_screen()` fills only the caller's viewport. Newline handling
keeps cursor X at local zero rather than physical zero.

No resize event is emitted. Applications are expected to query dimensions in
their rendering loop:

```c
for (;;) {
    int width = display__width();
    int height = display__height();

    if (width > 0 && height > 0 && display__begin_frame() == BRUCE_OK) {
        draw_application(width, height);
        display__present();
    }

    runtime__delay(50);
}
```

When Core assigns a new tile, it clears that rectangle. The tile may remain
blank until the application's next loop iteration redraws it.

## Tile Management API

The launcher is a module and may include only `core_sdk/...` headers. Add a
publicly declared but built-in-only layout operation:

```c
#define BRUCE_DISPLAY_MAX_TILES 4

typedef struct {
    int16_t x;
    int16_t y;
    int16_t width;
    int16_t height;
} bruce_display_rect_t;

typedef struct {
    bruce_process_id_t process_id;
    bruce_display_rect_t rect;
} bruce_display_tile_t;

bruce_result_t display__set_tiles(const bruce_display_tile_t *tiles,
                                  size_t count);
```

Validate that count is at most four, IDs are live and unique, rectangles are
positive and in bounds, and rectangles do not overlap. Reject external ELF and
JavaScript callers. Do not export this function through the ELF symbol table.

The launcher computes rectangles so it can reserve its own status bar, labels,
selection borders, and gutters. Core owns clipping and transfer policy, not
launcher visual design.

Changing a layout must acquire or reserve all affected regions in a stable
order. Return `BRUCE_ERR_BUSY` if an affected process owns an unfinished frame;
the launcher can retry in its loop. Never silently move a viewport midway
through a frame.

## Display Worker and DMA

Create one Core display worker as the sole panel-transfer owner.

The worker handles:

- Fullscreen presentation requests.
- Packed tile presentation requests.
- Notification appearance, replacement, dismissal, and expiration.
- Transfer submission and completion.
- Lease release and caller wakeup.

During panel setup, register
`esp_lcd_panel_io_callbacks_t.on_color_trans_done`. The callback should only
give an ISR-safe completion semaphore and request a context switch if needed.
It must not acquire Core locks.

Reduce `trans_queue_depth` to one unless another verified use requires queued
panel transactions. The transfer buffer and source framebuffer must not be
modified until the matching completion callback runs.

## Packed Partial Transfers

A tile is not contiguous in the physical framebuffer because each row has the
full-screen stride. Do not pass a pointer into the first tile pixel directly to
`esp_lcd_panel_draw_bitmap()`.

Use one shared DMA-capable packed buffer owned by the display worker:

```c
for (int row = 0; row < rect.height; ++row) {
    memcpy(&dma_buffer[row * rect.width],
           &framebuffer[(rect.y + row) * screen_width + rect.x],
           rect.width * sizeof(uint16_t));
}
```

A quarter-screen buffer is about 16 KB. It is shared transfer scratch, not a
private process render surface.

Fullscreen transfers may use the framebuffer directly when no overlay needs
composition. The fullscreen logical lease prevents mutation until DMA
completion.

## Logical Region Locking

Use these lock classes:

- A short-lived display state mutex.
- Core-owned logical viewport leases.
- A display-worker request queue.
- One panel transaction/completion state owned by the worker.

Do not hold an application-owned mutex from begin to present. Each primitive
briefly locks display state, validates the active logical lease, writes pixels,
and unlocks.

The display worker retains the logical lease while it packs and transfers.
Non-overlapping tiles may continue composing their next frames, but only one
SPI transfer occurs at a time.

## Rotation

Rotation remains global. `display__set_rotation()` must:

1. Wait for panel transfer completion.
2. Reject or wait for active frames.
3. Reconfigure panel swap, mirror, and gap settings.
4. Update physical logical dimensions.
5. Clear the shared framebuffer instead of reinterpreting its old stride.
6. Hide existing tiled assignments.
7. Increment affected viewport generations.
8. Re-anchor any active notification.
9. Let the launcher submit a new layout.

External tiled processes must not be allowed to rotate the physical panel.

## Dialog Migration

Wrap GUI redraws in explicit frames. Relevant locations include:

- Message renderer: `src/core/dialog/dialog.c:264-303`.
- Choice renderer: `src/core/dialog/dialog.c:306-362`.
- Keyboard renderer: `src/core/dialog/dialog.c:595-695`.
- Text viewer renderer: `src/core/dialog/dialog.c:1025-1085`.

A dialog that loses foreground must treat `BRUCE_ERR_NOT_FOREGROUND` as
cancellation instead of spinning.

## JavaScript and ELF

Add JavaScript methods:

```text
display.beginFrame()
display.present()
display.flush()
```

Keep existing names in `src/modules/loaders/js/mqjs_stdlib.c:647-691`.
Long-running scripts should use explicit frames. A compatibility presentation
after top-level script or `app_main()` return may be needed for scripts that
draw only once.

The ELF resolver currently exports no display symbols despite `display.h`
being public. Update:

- `src/modules/loaders/elf/elf_loader_sdk_symbols.c`.
- `elf_apps/include/bruce_sdk.h`.
- `elf_apps/README.md`.

Export existing display primitives plus begin, present, and flush. Do not
export tile management.

## Files Expected to Change

- `src/CMakeLists.txt`
- `src/core_sdk/display.h`
- `src/core/display/display.h`
- `src/core/display/display.c`
- `src/core/process/process.c`
- `src/core/dialog/dialog.c`
- `src/modules/bruce_launcher/bruce_launcher_app.c`
- `src/modules/loaders/js/display_js.c`
- `src/modules/loaders/js/display_js.h`
- `src/modules/loaders/js/mqjs_stdlib.c`
- `src/modules/loaders/elf/elf_loader_sdk_symbols.c`
- `elf_apps/include/bruce_sdk.h`
- `elf_apps/README.md`
- `ARCHITECTURE.md`
- `migration_plan.md`

New sources must be added explicitly to `src/CMakeLists.txt`; this component
does not glob sources.

## Tests

Add display/compositor selftests covering:

- Foreground fullscreen dimensions.
- Hidden zero dimensions and no-op drawing.
- One through four tile assignments.
- Portrait and landscape geometry.
- Local-coordinate translation.
- Clipping and tile-local fill-screen behavior.
- Independent text and cursor state.
- Nested begin-frame rejection.
- Viewport generation validation.
- Layout changes rejected during active frames.
- Packed row correctness.
- Lease retention through mocked DMA completion.
- Submission failure cleanup.
- Process exit and force-kill cleanup.
- Rotation behavior.
- Existing flush compatibility.

Use a private panel transport hook or test seam so state-machine tests do not
require physical LCD DMA.

## Implementation Order

1. Complete the foreground input handoff fix.
2. Add process-local display contexts and hidden/fullscreen behavior.
3. Add Core-owned logical frame leases.
4. Add the display worker and transfer-completion callback.
5. Add packed partial rectangle transfers.
6. Add transactional tile assignment.
7. Migrate dialogs and launcher drawing to frame transactions.
8. Implement the launcher process overview.
9. Add notification composition.
10. Add the status-icon registry.
11. Add JavaScript, ELF, and terminal front ends.
12. Complete selftests and contract updates.

## Verification

Build the firmware:

```bash
bash -o pipefail -c 'source ~/esp/idf/export.sh && idf.py build 2>&1 | tail -n 200'
```

Build SDK-only smoke targets:

```bash
cmake --build build --target \
  bruce_sdk_builtin_launcher_check \
  bruce_sdk_builtin_wifi_check \
  bruce_sdk_builtin_utils_launcher_check \
  bruce_sdk_builtin_utils_terminal_check \
  bruce_sdk_builtin_elf_loader_check \
  bruce_sdk_builtin_js_loader_check
```

Regenerate the JavaScript stdlib after changing its registration table:

```bash
cmake --build build --target mquickjs_generate_stdlib
```

Build ELF examples:

```bash
source ~/esp/idf/export.sh
python3 elf_apps/tools/build_elf_apps.py --target esp32s3
```

Firmware selftests must also be run on hardware after flashing.
