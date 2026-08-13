#include "process_internal.h"

#include "core/display/display.h"
#include "core_sdk/environment.h"
#include "core_sdk/permission.h"

#include <limits.h>
#include <string.h>

#include "esp_timer.h"


bruce_process_id_t process__current_id(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_process_id_t id = self != NULL ? self->id : BRUCE_PROCESS_ID_INVALID;
    process__unlock();
    return id;
}

bruce_process_signal_t process__current_signal(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_process_signal_t signal =
        self != NULL && self->stop_requested ? self->pending_signal : (bruce_process_signal_t)0;
    process__unlock();
    return signal;
}

bruce_result_t process__list(bruce_process_snapshot_t *snapshots, size_t capacity, size_t *out_count) {
    process__ensure_init();
    if (out_count == NULL || (capacity != 0 && snapshots == NULL)) { return BRUCE_ERR_INVALID_ARGUMENT; }
    process__lock();
    process__refresh_cpu_samples_locked();
    size_t written = 0;
    for (process__record_t *record = s_processes; record != NULL && written < capacity; record = record->next) {
        if (record->in_use) {
            process__fill_snapshot_locked(record, &snapshots[written]);
            written++;
        }
    }
    *out_count = written;
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__snapshot(bruce_process_id_t process_id, bruce_process_snapshot_t *out_snapshot) {
    process__ensure_init();
    if (out_snapshot == NULL) { return BRUCE_ERR_INVALID_ARGUMENT; }
    process__lock();
    process__refresh_cpu_samples_locked();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    process__fill_snapshot_locked(record, out_snapshot);
    process__unlock();
    return BRUCE_OK;
}

static bruce_result_t process__switch_relative(int direction) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    bruce_process_id_t self_id = self != NULL ? self->id : BRUCE_PROCESS_ID_INVALID;
    bruce_process_id_t anchor_id = s_effective_foreground;
    process__record_t *anchor = process__find_by_id_locked(anchor_id);
    if (anchor == NULL || !anchor->presentable) {
        anchor_id = BRUCE_PROCESS_ID_INVALID;
        for (process__record_t *stacked = s_fg_tail; stacked != NULL; stacked = stacked->fg_previous) {
            if (stacked->presentable) {
                anchor_id = stacked->id;
                break;
            }
        }
    }

    process__record_t *anchor_record = process__find_by_id_locked(anchor_id);
    process__record_t *candidate = anchor_record;
    process__record_t *start = NULL;
    for (;;) {
        if (candidate == NULL) candidate = direction > 0 ? s_processes : s_process_tail;
        else candidate = direction > 0 ? candidate->next : candidate->previous;
        if (candidate == NULL) candidate = direction > 0 ? s_processes : s_process_tail;
        if (candidate == NULL) break;
        if (start == NULL) start = candidate;
        else if (candidate == start) break;
        if (candidate->in_use && candidate->id != self_id && candidate->presentable &&
            candidate->state == BRUCE_PROCESS_BACKGROUND) {
            process__foreground_push_locked(candidate->id);
            process__unlock();
            return BRUCE_OK;
        }
    }
    process__unlock();
    return BRUCE_ERR_NOT_FOUND;
}

bruce_result_t process_registry__switch_next(void) { return process__switch_relative(1); }

bruce_result_t process_registry__switch_previous(void) { return process__switch_relative(-1); }

bruce_process_id_t process_registry__foreground_id(void) {
    process__ensure_init();
    process__lock();
    bruce_process_id_t process_id = s_effective_foreground;
    process__unlock();
    return process_id;
}

void process_registry__mark_presentable(bruce_process_id_t process_id) {
    if (process_id == BRUCE_PROCESS_ID_INVALID) return;
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record != NULL) {
        record->presentable = true;
        if (!record->gui_requested) {
            record->gui_requested = true;
            display__process_set_gui_requested(record->id);
        }
    }
    process__unlock();
}

bruce_result_t process__switch_next(void) {
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission_result != BRUCE_OK) return permission_result;
    return process_registry__switch_next();
}

bruce_result_t process__switch_previous(void) {
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission_result != BRUCE_OK) return permission_result;
    return process_registry__switch_previous();
}

bruce_result_t process__to_background(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->state != BRUCE_PROCESS_FOREGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    process__foreground_remove_locked(self->id);
    self->state = BRUCE_PROCESS_BACKGROUND;
    display__process_state_changed(self->id, self->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__to_foreground(void) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (self == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (self->state != BRUCE_PROCESS_BACKGROUND && self->state != BRUCE_PROCESS_FOREGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    if (!self->gui_requested) {
        self->gui_requested = true;
        display__process_set_gui_requested(self->id);
    }
    bool promoted = self->state == BRUCE_PROCESS_BACKGROUND;
    if (promoted) process__foreground_push_locked(self->id);
    process__unlock();
    if (promoted) process__clear_foreground_display();
    return BRUCE_OK;
}

bruce_result_t process__foreground(bruce_process_id_t process_id) {
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *target = process__find_by_id_locked(process_id);
    if (target == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (target->state == BRUCE_PROCESS_FOREGROUND) {
        process__unlock();
        return BRUCE_OK;
    }
    if (target->state != BRUCE_PROCESS_BACKGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    process__foreground_push_locked(process_id);
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__signal(bruce_process_id_t process_id, bruce_process_signal_t signal) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX ||
        (signal != BRUCE_PROCESS_SIGNAL_INT && signal != BRUCE_PROCESS_SIGNAL_KILL &&
         signal != BRUCE_PROCESS_SIGNAL_TERM)) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (signal == BRUCE_PROCESS_SIGNAL_KILL) { return process__kill(process_id); }
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    record->stop_requested = true;
    record->pending_signal = signal;
    record->state = BRUCE_PROCESS_STOPPING;
    if (record->process_entry_stop != NULL) {
        /* Stop hooks are non-blocking runtime interruption requests. Calling
         * under the registry lock keeps their owned context alive. */
        record->process_entry_stop(record->process_entry_context, signal);
    }
    process__wake_locked(record);
    xEventGroupSetBits(record->events, PROCESS__EVT_EVENT_WAKE);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__terminate(bruce_process_id_t process_id) {
    return process__signal(process_id, BRUCE_PROCESS_SIGNAL_TERM);
}

bruce_result_t process__pause(bruce_process_id_t process_id) {
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (record->state == BRUCE_PROCESS_PAUSED) {
        process__unlock();
        return BRUCE_OK;
    }
    if (record->state != BRUCE_PROCESS_FOREGROUND && record->state != BRUCE_PROCESS_BACKGROUND) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    record->state = BRUCE_PROCESS_PAUSED;
    record->pause_requested = true;
    process__wake_locked(record);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__resume(bruce_process_id_t process_id) {
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    if (record->state != BRUCE_PROCESS_PAUSED) {
        process__unlock();
        return BRUCE_ERR_INVALID_STATE;
    }
    record->pause_requested = false;
    record->state = BRUCE_PROCESS_BACKGROUND;
    process__wake_locked(record);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();
    process__unlock();
    return BRUCE_OK;
}

bruce_result_t process__kill(bruce_process_id_t process_id) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (process_id != process__current_id()) {
        bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
        if (permission_result != BRUCE_OK) return permission_result;
    }
    process__ensure_init();
    process__lock();
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    TaskHandle_t handle = record->handle;
    bool is_self = handle != NULL && handle == xTaskGetCurrentTaskHandle();

    if (is_self) {
        /* Self-kill: tear down first, then delete; this call never returns. */
        bruce_process_status_t status = {
            .reason = BRUCE_PROCESS_KILLED,
            .exit_code = 0,
            .signal = BRUCE_PROCESS_SIGNAL_KILL,
        };
        process__teardown_locked(record, &status);
        process__unlock();
        vTaskDelete(NULL);
        return BRUCE_OK; /* unreachable */
    }

    /* Close Core service gates before deletion. The display hook waits past
     * any short raster critical section and transfers an in-flight frame to
     * worker ownership; foreground recomputation similarly revokes input. */
    record->stop_requested = true;
    record->pending_signal = BRUCE_PROCESS_SIGNAL_KILL;
    record->state = BRUCE_PROCESS_STOPPING;
    process__wake_locked(record);
    xEventGroupSetBits(record->events, PROCESS__EVT_EVENT_WAKE);
    display__process_state_changed(record->id, record->state);
    process__foreground_recompute_locked();

    if (record->operation_count > 0) {
        process__unlock();
        (void)xEventGroupWaitBits(
            record->events, PROCESS__EVT_OPERATION_IDLE, pdFALSE, pdTRUE, portMAX_DELAY
        );
        process__lock();
        record = process__find_by_id_locked(process_id);
        if (record == NULL) {
            process__unlock();
            return BRUCE_OK;
        }
        handle = record->handle;
    }

    /* Arbitrary application-owned mutexes cannot be recovered after a force
     * delete, which remains the documented limitation of process__kill(). */
    if (handle != NULL) {
        vTaskSetThreadLocalStoragePointer(handle, PROCESS__TLS_SLOT, NULL);
        vTaskDelete(handle);
    }
    bruce_process_status_t status = {
        .reason = BRUCE_PROCESS_KILLED,
        .exit_code = 0,
        .signal = BRUCE_PROCESS_SIGNAL_KILL,
    };
    process__teardown_locked(record, &status);
    process__unlock();
    return BRUCE_OK;
}

static bruce_result_t
process__wait_common(bruce_process_id_t process_id, uint32_t timeout_ms, bruce_process_status_t *out_status) {
    process__ensure_init();
    process__lock();
    process__completion_t *completion = process__find_completion_locked(process_id);
    if (completion != NULL) {
        if (out_status != NULL) {
            bruce_process_status_t status = completion->status;
            process__completion_clear_locked(completion);
            *out_status = status;
        }
        process__unlock();
        return BRUCE_OK;
    }
    process__record_t *record = process__find_by_id_locked(process_id);
    if (record == NULL) {
        process__unlock();
        return BRUCE_ERR_NOT_FOUND;
    }
    record->waiter_count++;
    if (out_status != NULL) record->status_waiter_count++;
    process__record_t *waiter = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    if (waiter != NULL) {
        waiter->wait_attached = true;
        waiter->wait_target = record;
        waiter->wait_for_status = out_status != NULL;
    }
    bool cancelled = waiter != NULL && waiter->stop_requested;
    EventGroupHandle_t events = record->events;
    process__unlock();

    int64_t deadline_us =
        timeout_ms == UINT32_MAX ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    EventBits_t bits = 0;
    for (;;) {
        TickType_t ticks = portMAX_DELAY;
        if (timeout_ms != UINT32_MAX) {
            int64_t remaining_us = deadline_us - esp_timer_get_time();
            if (remaining_us <= 0) {
                ticks = 0;
            } else {
                uint64_t remaining_ms = ((uint64_t)remaining_us + 999u) / 1000u;
                ticks = pdMS_TO_TICKS(remaining_ms);
                if (ticks == 0) ticks = 1;
            }
        }
        if (!cancelled) {
            bits |= xEventGroupWaitBits(
                events, PROCESS__EVT_EXITED | PROCESS__EVT_WAITER_WAKE, pdTRUE, pdFALSE, ticks
            );
        }

        process__lock();
        completion = process__find_completion_locked(process_id);
        cancelled = waiter != NULL && waiter->stop_requested;
        bool absent = !record->in_use;
        bool timed_out = timeout_ms != UINT32_MAX && esp_timer_get_time() >= deadline_us;
        if (completion != NULL || cancelled || absent || timed_out) break;
        process__unlock();
    }

    bruce_result_t result = BRUCE_ERR_TIMEOUT;
    bruce_process_status_t status;
    if (out_status != NULL && completion != NULL) {
        status = completion->status;
        process__completion_clear_locked(completion);
        result = BRUCE_OK;
    } else if (out_status == NULL && (completion != NULL || (bits & PROCESS__EVT_EXITED) != 0)) {
        result = BRUCE_OK;
    } else if (cancelled) {
        result = BRUCE_ERR_CANCELLED;
    } else if (!record->in_use) {
        /* Another status waiter may have consumed the completion first. */
        result = BRUCE_ERR_NOT_FOUND;
    }

    if (waiter != NULL) {
        process__detach_wait_locked(waiter);
    } else {
        if (out_status != NULL && completion != NULL && completion->in_use && completion->waiter_pins > 0) {
            completion->waiter_pins--;
        }
        if (record->waiter_count > 0) record->waiter_count--;
        if (out_status != NULL && record->status_waiter_count > 0) {
            record->status_waiter_count--;
        }
        process__dispose_if_unused_locked(record);
    }
    process__unlock();

    if (result == BRUCE_OK && out_status != NULL) *out_status = status;
    return result;
}

bruce_result_t process__wait(bruce_process_id_t process_id, uint32_t timeout_ms) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (process_id == process__current_id()) { return BRUCE_ERR_INVALID_STATE; }
    return process__wait_common(process_id, timeout_ms, NULL);
}

bruce_result_t
process__wait_status(bruce_process_id_t process_id, uint32_t timeout_ms, bruce_process_status_t *out_status) {
    if (process_id == BRUCE_PROCESS_ID_INVALID || process_id > (bruce_process_id_t)INT_MAX ||
        out_status == NULL) {
        return BRUCE_ERR_INVALID_ARGUMENT;
    }
    if (process_id == process__current_id()) { return BRUCE_ERR_INVALID_STATE; }
    bruce_result_t permission_result = permission__check(BRUCE_PERMISSION_PROCESS);
    if (permission_result != BRUCE_OK) return permission_result;
    return process__wait_common(process_id, timeout_ms, out_status);
}

/* Shared implementation for runtime__sleep()/runtime__delay(): blocks while
 * paused (until resumed or stopped), returns BRUCE_ERR_CANCELLED as soon as a
 * stop is requested, and otherwise waits out `ms`.  When `interruptible` is
 * true and the process is background when the wait begins, being foregrounded
 * mid-wait also returns BRUCE_ERR_CANCELLED early. */
static bruce_result_t process__wait_ms(uint32_t ms, bool interruptible) {
    process__ensure_init();
    process__lock();
    process__record_t *self = process__find_by_handle_locked(xTaskGetCurrentTaskHandle());
    process__unlock();
    if (self == NULL) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return BRUCE_OK;
    }
    process__lock();
    bool was_background = self->state == BRUCE_PROCESS_BACKGROUND;
    process__unlock();

    int64_t deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        process__lock();
        bool stopped = self->stop_requested;
        bool paused = self->pause_requested;
        bool now_foreground = self->state == BRUCE_PROCESS_FOREGROUND;
        process__unlock();

        if (stopped) { return BRUCE_ERR_CANCELLED; }
        if (paused) {
            xEventGroupWaitBits(self->events, PROCESS__EVT_WAKE, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        if (interruptible && was_background && now_foreground) { return BRUCE_ERR_CANCELLED; }

        int64_t remaining_us = deadline_us - esp_timer_get_time();
        if (remaining_us <= 0) { return BRUCE_OK; }
        uint64_t remaining_ms = ((uint64_t)remaining_us + 999u) / 1000u;
        TickType_t wait_ticks = pdMS_TO_TICKS(remaining_ms);
        if (wait_ticks == 0) wait_ticks = 1;
        (void)xEventGroupWaitBits(self->events, PROCESS__EVT_WAKE, pdTRUE, pdFALSE, wait_ticks);
        /* Woken early; loop to re-check stop/pause/foreground state. */
    }
}

uint64_t runtime__now(void) { return (uint64_t)esp_timer_get_time() / 1000u; }

bruce_result_t runtime__sleep(uint32_t milliseconds) { return process__wait_ms(milliseconds, true); }

bruce_result_t runtime__delay(uint32_t milliseconds) { return process__wait_ms(milliseconds, false); }

bool runtime__gui_requested(void) {
    const char *value = environment__get("GUI");
    return value != NULL && strcmp(value, "1") == 0;
}
