# Notifications and Status Icons Plan

> **Superseded (notification half only):** this document's decision to keep
> notification composition inside Core display (`core/display`) has been
> replaced. Core now exposes only a generic `display__overlay_*` primitive
> (see `core_sdk/display.h`); the notification banner itself moved to
> `modules/notification_service`, a background service built on that primitive, with
> `notification__push()`/`notification__dismiss()` reduced to a mailbox in
> `core/notification`. See `ARCHITECTURE.md`'s display/overlay and
> notification paragraphs for the current design. The status-icon half below
> is unaffected and still accurate.

## Scope

This document defines two separate services:

- A Core-composited, transient notification shown in the physical bottom-right
  corner.
- A global keyed status-icon registry that Core stores but does not render.

Both services are intentionally unrestricted. Built-in, ELF, and JavaScript
applications may use them without permission checks.

## Current State

BruceIDF has no notification or status-icon service.

- `src/modules/loaders/js/mqjs_stdlib.c:369-375` contains a commented legacy
  notification object with only a proposed blink function.
- `src/modules/loaders/js/dialog_js.c:449-455` exposes a no-op status-bar
  compatibility binding.
- `src/modules/bruce_launcher/bruce_launcher_app.c:152-172` draws a fixed
  launcher status region but has no icon registry.
- `ARCHITECTURE.md:431-432` currently says there is no compositor.

The legacy centered severity stripe is a visual reference only. Do not copy
legacy implementation code into BruceIDF.

## Transient Notification API

Add `src/core_sdk/notification.h`:

```c
#define BRUCE_NOTIFICATION_TEXT_MAX 96

bruce_result_t notification__push(const char *text,
                                  uint32_t duration_ms);

bruce_result_t notification__dismiss(void);
```

Policy:

- No permission check.
- Any process can show a notification.
- Any process can replace the current notification.
- Any process can dismiss it.
- Last writer wins; there is no queue or retained inbox.
- Dismiss is idempotent.
- The text is copied synchronously into fixed Core storage.
- Duration is clamped to a documented minimum and maximum.
- No input is captured.
- Producer exit does not dismiss the notification.
- Expiration occurs even if no application presents another frame.

The implementation may later add severity or title fields, but v1 should keep
the payload minimal unless the UI design requires them immediately.

## Notification State

Keep state owned by the compositor/display service:

```c
typedef struct {
    bool active;
    char text[BRUCE_NOTIFICATION_TEXT_MAX];
    TickType_t expires_at;
    bruce_display_rect_t rect;
    uint32_t generation;
} notification__state_t;
```

`notification__push()` copies text, updates expiration, increments generation,
and wakes the display worker. Generation checking prevents an old expiration
from dismissing a replacement notification.

## Composition

The notification is anchored to the physical logical screen's bottom-right
corner. It is not relative to an application tile.

Do not permanently draw it into the shared application framebuffer.

For independent notification updates, the display worker must:

1. Wait until logical leases intersecting the notification rectangle are free.
2. Copy underlying pixels into packed DMA scratch.
3. Draw the notification into scratch.
4. Submit the rectangle and wait for transfer completion.
5. Leave the application framebuffer unchanged.

For a normal tile or fullscreen presentation intersecting an active
notification, compose the notification into the outgoing transfer data. This
prevents an application update from visually erasing the overlay.

On expiration or dismissal, transfer the current underlying framebuffer
rectangle without the notification.

Keep v1 event-driven. Do not implement fades, scrolling, countdown redraws, or
a continuous compositor loop. Display traffic occurs on show, replacement,
application presentation, dismissal, and expiration.

## Status Icon API

Add `src/core_sdk/status_icon.h`:

```c
#define BRUCE_STATUS_ICON_KEY_MAX 32
#define BRUCE_STATUS_ICON_MAX 16
#define BRUCE_STATUS_ICON_MAX_WIDTH 16
#define BRUCE_STATUS_ICON_MAX_HEIGHT 16
#define BRUCE_STATUS_ICON_BITMAP_MAX 32

typedef struct {
    char key[BRUCE_STATUS_ICON_KEY_MAX];
    uint8_t width;
    uint8_t height;
    uint8_t bitmap[BRUCE_STATUS_ICON_BITMAP_MAX];
} bruce_status_icon_t;

bruce_result_t status_icon__push(const char *key,
                                 const uint8_t *bitmap,
                                 uint8_t width,
                                 uint8_t height);

bruce_result_t status_icon__remove(const char *key);

bruce_result_t status_icon__list(bruce_status_icon_t *icons,
                                 size_t capacity,
                                 size_t *out_count,
                                 uint32_t *out_revision);
```

The bitmap format is 1bpp, row-major, MSB-first, with each row byte-aligned.
Core validates that the supplied dimensions fit fixed storage and copies only
the required bytes.

## Status Registry Policy

- No permission check.
- The namespace is global.
- No source or owner is stored.
- Any process can replace any key.
- Any process can remove any key.
- Removal is idempotent.
- Pushing an existing key replaces it without consuming another slot.
- Adding a new key to a full registry returns `BRUCE_ERR_RESOURCE_LIMIT`.
- Entries survive producer process exit.
- Entries are runtime-only and are not persisted.
- Every effective mutation increments a revision counter.
- Listing returns a stable, deterministic order.
- Core never automatically draws status icons.

Use lexicographic key ordering for deterministic display unless an explicit
ordering field is introduced. The unrestricted replacement and removal policy
is intentional and must be stated in `ARCHITECTURE.md`.

## Rendering Responsibility

The launcher calls `status_icon__list()` and draws icons in its own top status
bar using launcher theme colors. A fullscreen application may list and render
the same icons if it chooses to reserve status-bar space.

Core does not reserve a global top bar. Applications may use the full screen.
The launcher controls whether its own tiled layout leaves space for status
icons.

The launcher should poll the revision in its existing loop and redraw the
status region only when the revision changes. It should not redraw all process
tiles for an icon-only change.

## Concurrency

Protect notification state and the status registry with Core state locks.
Copy list snapshots while locked, then release the lock before drawing.

Follow the global lock order documented by the compositor:

```text
process registry -> display/compositor -> notification/status state
```

Prefer folding notification state into compositor state to avoid another lock.
The status registry can be independent because rendering is application-owned.

No timer callback may render or wait for DMA. Timer expiration should only
signal the display worker.

## JavaScript

Expose:

```js
notification.push(text, durationMs)
notification.dismiss()

statusIcon.push(key, bitmap, width, height)
statusIcon.remove(key)
statusIcon.list()
```

Add binding source files to both the main component source list and the JS SDK
compile-smoke target in `src/CMakeLists.txt`.

Update `src/modules/loaders/js/mqjs_stdlib.c`; do not edit generated
`mqjs_stdlib.h` manually. Regenerate it through the CMake target.

Validate JavaScript bitmap buffer length before passing data to Core.

## ELF

Add both headers to `elf_apps/include/bruce_sdk.h` and export all notification
and status-icon functions from
`src/modules/loaders/elf/elf_loader_sdk_symbols.c`.

The APIs are intentionally available without permission checks.

## Terminal

Provide thin terminal-accessible commands over the same Core APIs:

```text
notify push <duration-ms> <text>
notify dismiss
notify icon-list
notify icon-remove <key>
```

Bitmap push is optional in the terminal because entering binary bitmap data is
awkward. Do not add feature-specific parsing to the generic terminal loop;
register a built-in command through AppRunner.

`APP_RUNNER_MAX_APPS` is 16, leaving capacity beyond the seven defaults and
selftest commands for this built-in.

## Files Expected to Change

- New `src/core_sdk/notification.h`
- New `src/core_sdk/status_icon.h`
- New Core implementation files under `src/core/`
- `src/CMakeLists.txt`
- `src/core/display/display.c`
- `src/core/display/display.h`
- `src/modules/bruce_launcher/bruce_launcher_app.c`
- `src/modules/loaders/js/mqjs_stdlib.c`
- New JavaScript binding sources
- `src/modules/loaders/elf/elf_loader_sdk_symbols.c`
- `elf_apps/include/bruce_sdk.h`
- `elf_apps/README.md`
- `src/modules/selftest/selftest.c`
- New selftest sources
- `ARCHITECTURE.md`
- `migration_plan.md`

## Tests

Notification tests must cover:

- Text copying.
- Duration validation and clamping.
- Last-writer replacement.
- Cross-process dismissal.
- Idempotent dismissal.
- Expiration generation handling.
- Bottom-right placement in every rotation.
- Non-destructive composition.
- Restoration using current underlying pixels.
- Intersection with fullscreen and tile transfers.
- Producer exit while notification remains active.

Status-icon tests must cover:

- Insert and list.
- Replacement by key.
- Cross-process replacement and removal.
- Idempotent removal.
- Key and bitmap validation.
- Capacity limit.
- Replacement while full.
- Stable ordering.
- Revision changes.
- Persistence after producer exit.
- Concurrent list and mutation.

## Acceptance Criteria

- Notification APIs and status-icon APIs perform no permission checks.
- A notification never permanently changes application framebuffer pixels.
- Notification expiration works without application redraw activity.
- An application presentation cannot visually erase an active notification.
- Any process can replace or dismiss the current notification.
- Any process can push, replace, list, or remove any status icon.
- Status icons are rendered only by launcher/apps that request the registry.
- No continuous redraw loop is introduced.
