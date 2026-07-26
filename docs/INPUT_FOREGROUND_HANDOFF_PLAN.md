# Foreground Input Handoff Fix

## Problem

`input__read()` currently checks whether its caller is foreground and then
blocks directly on one shared FreeRTOS queue:

- Foreground check: `src/core/input/input.c:650-655`.
- Blocking dequeue: `src/core/input/input.c:657-662`.
- Queue declaration and storage: `src/core/input/input.c:120-128`.

This sequence is racy:

```text
Task A verifies that it is foreground.
Task A blocks in xQueueReceive().
Task B becomes foreground.
An event is queued.
FreeRTOS wakes Task A because it is an older queue waiter.
Task A removes the event intended for Task B.
```

Checking foreground again after dequeue does not fix the issue. The old task
has already removed the event. Requeueing is also unsafe because ordering and
capacity may have changed.

The compositor task switcher requires immediate, race-free physical-input
handoff.

## Required Semantics

- Only the effective foreground task can remove physical input events.
- Losing foreground revokes an already-blocked read immediately.
- The revoked call returns `BRUCE_ERR_NOT_FOREGROUND`.
- Regaining foreground does not revive an old read.
- A new read under the new foreground tenure can receive events.
- Pause, stop, exit, and kill immediately relinquish input ownership.
- Timeout duration is measured across internal wakes, not restarted by them.
- Input deinitialization wakes blocked readers.

## Foreground Lease

Add input-owned state protected by the input mutex:

```c
static bruce_task_id_t s_foreground_task_id;
static uint32_t s_foreground_epoch;
```

Every effective owner change increments the epoch, including `A -> B -> A`.
This prevents Task A's old blocked call from becoming valid merely because A
later becomes foreground again.

Task transitions update input ownership through a private Core hook. The task
registry remains authoritative for foreground policy; input mirrors only the
effective owner and epoch needed to linearize queue removal.

## Wait Architecture

Keep the single global event queue, but never perform a blocking receive on it.

Add a dedicated per-task input wake bit to the existing task event groups:

```c
#define TASK__EVT_INPUT_WAKE (1u << 2)
```

Do not reuse `TASK__EVT_WAKE`; runtime sleep and delay consume that bit with
different semantics.

`input__read()` should use this loop:

1. Resolve the calling task ID.
2. Capture its current foreground epoch.
3. Clear the caller's input-wake bit.
4. Lock the input mutex.
5. Validate initialization, owner ID, and epoch.
6. Call `xQueueReceive(s_queue, out_event, 0)`.
7. Unlock the input mutex.
8. Return the event if one was received.
9. Wait on the input-wake bit for the remaining timeout.
10. Repeat validation and nonblocking dequeue.

The clear-check-wait ordering prevents missed wakes:

- An event before the clear is found by the queue check.
- An event between clear and check is found by the queue check.
- An event after the check sets the wake bit.

The input mutex is the linearization point between foreground transitions and
event removal.

## Producer Changes

Every producer must:

1. Build metadata before taking the input mutex.
2. Lock input.
3. Enqueue with zero wait.
4. Capture the current foreground owner.
5. Unlock input.
6. Wake that owner's input-wake bit.

Apply this to physical polling and `input__inject()`.

`input__inject()` currently bypasses the input mutex at
`src/core/input/input.c:786-812`. This can interleave with `input__check()`,
which temporarily drains and reconstructs the queue at
`src/core/input/input.c:744-782`. Injection must use the same mutex.

`input__push_event_locked()` currently calls `task__current_id()` while input
is locked. Remove that lock inversion by resolving or passing the source task
ID before taking the input mutex. Physical polling should use
`BRUCE_TASK_ID_INVALID` as its source task.

## Nonblocking Input APIs

Move foreground validation inside the same input mutex used for queue access in:

- `input__flush()`.
- `input__peek()`.
- `input__check()`.
- Zero-time `input__read()` and polling wrappers.

This makes each operation occur wholly before or wholly after a foreground
transition.

`input__wait()` must preserve one total deadline while discarding release or
change events. It must propagate foreground revocation immediately.

## Foreground Stack Repair

The existing stack implementation has issues that affect reliable handoff:

- It removes only the top task at `src/core/task/task.c:136-152`.
- A buried task can exit and leave a stale ID.
- Foregrounding a buried task can duplicate its ID.
- The stack has depth eight while the registry has sixteen task records.
- Overflow silently marks a task foreground without placing it on the stack.
- Paused or stopping records can interfere with restoration.

Replace scattered state assignment with one task-locked recomputation path.

Required invariants:

1. A task ID appears at most once in the stack.
2. Every stack ID refers to a live task.
3. Stack capacity is at least `TASK__MAX_RECORDS`.
4. The effective foreground is the highest live, unpaused, non-stopping entry.
5. Exactly that record has `BRUCE_TASK_FOREGROUND`.
6. Input's mirrored foreground ID equals the effective foreground.
7. Removing a task works from any stack position.
8. Every effective-owner change increments the input epoch.

Use the common path for:

- Initial foreground task start.
- `task__foreground()`.
- `task__to_background()`.
- Pause and resume.
- Stop.
- Normal exit.
- Force kill.

Pause may preserve stack position while making the record temporarily
ineligible. Resume recomputes the effective owner instead of blindly restoring
`state_before_pause`.

Stop should immediately make the record ineligible so an underlying task can
receive input before cooperative teardown finishes.

## Locking

The required lock order is:

```text
task registry lock -> input mutex
```

Never acquire the task registry lock while holding the input mutex.

Never block on a queue, event group, or semaphore while holding either lock.

Private task helpers should clear, wait for, and signal input-wake bits without
exposing FreeRTOS handles through `core_sdk/task.h`.

Force kill needs special care because deleting a task while it owns a Core
mutex can strand that mutex. The implementation must establish a safe input
barrier or otherwise prove that the target cannot own/re-enter the input mutex
when it is deleted.

## Deinitialization

`input__deinit()` currently cannot wake a task blocked directly in the queue.
After the wait architecture changes, deinitialization must:

1. Mark input uninitialized under the input mutex.
2. Invalidate the foreground owner.
3. Increment the epoch.
4. Release the mutex.
5. Wake the previous owner's input-wake bit.

The blocked call then returns `BRUCE_ERR_NOT_INITIALIZED`.

## Public Documentation

Update `src/core_sdk/input.h` to state:

- Blocking reads are revoked when their foreground tenure ends.
- Revoked calls return `BRUCE_ERR_NOT_FOREGROUND`.
- Regaining foreground requires a new read call.
- Finite timeouts are total elapsed time across internal wakes.
- Infinite waits are interrupted by foreground loss and deinitialization.

Update `ARCHITECTURE.md:228-253` and `ARCHITECTURE.md:423-429` with the same
contract.

## Files Expected to Change

- `src/core/input/input.c`
- `src/core/input/input.h`
- `src/core/task/task.c`
- `src/core/task/task.h`
- `src/core_sdk/input.h`
- `src/modules/selftest/input_test.c`
- `src/modules/selftest/input_test.h`
- `src/modules/selftest/task_test.c`
- `ARCHITECTURE.md`

## Required Tests

### Blocking Handoff

Task A blocks indefinitely while foreground. Task B becomes foreground and
blocks. Inject a unique event. Assert that A returns
`BRUCE_ERR_NOT_FOREGROUND` and B receives the event.

### ABA Lease

Run `A -> B -> A` before A's original blocked call resumes. Assert that A's old
call is revoked and a new call receives the event.

### Background

Background a blocked foreground task. Assert that the restored task receives
the next event.

### Pause and Resume

Pause the foreground owner, deliver input to the restored task, resume the old
owner, and verify that only a fresh read under the new epoch works.

### Stop and Kill

Verify immediate revocation, restoration, no deadlock, and no stale stack entry
for both cooperative stop and force kill.

### Buried Removal

Build stack `[A, B, C]`, remove B, then remove C. Verify that A is restored and
receives input.

### Timeout

Verify zero timeout, finite timeout, infinite wait cancellation, and that
spurious wakes do not extend finite deadlines.

### Stress

Race foreground transitions with read, peek, flush, check, and injection.
Assert no duplication, stale-owner removal, ordering corruption, or deadlock.

## Acceptance Criteria

- A revoked input lease can never dequeue an event.
- The new foreground does not compete with stale queue waiters.
- `A -> B -> A` does not revive an old read.
- Pause and stop immediately relinquish input ownership.
- Exit and kill remove stack entries from any position.
- Injection cannot corrupt `input__check()` queue reconstruction.
- No input path takes task lock while holding input lock.
- No blocking wait occurs while holding a Core state lock.
- Force kill cannot strand the input mutex.
