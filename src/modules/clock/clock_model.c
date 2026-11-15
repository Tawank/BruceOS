#include "clock_model.h"

#include <string.h>

int64_t clock_alarm__next_fire(const clock_alarm_t *alarm, int64_t now_s) {
    int64_t fire_s = now_s - now_s % CLOCK_MODEL__SECONDS_PER_DAY + alarm->seconds_of_day;
    return fire_s > now_s ? fire_s : fire_s + CLOCK_MODEL__SECONDS_PER_DAY;
}

static bool clock_stopwatch__in_use(const clock_stopwatch_t *stopwatch) {
    return stopwatch->running || stopwatch->elapsed_ms > 0;
}

bool clock_model__is_active(const clock_model_t *model) {
    for (size_t i = 0; i < model->alarm_count; ++i) {
        if (model->alarms[i].enabled) return true;
    }
    return model->timer_count > 0 || clock_stopwatch__in_use(&model->stopwatch);
}

uint32_t clock_timer__remaining_s(const clock_timer_t *timer, uint64_t now_ms) {
    uint32_t remaining_ms = timer->remaining_ms;
    if (timer->running) remaining_ms = now_ms < timer->deadline_ms ? (uint32_t)(timer->deadline_ms - now_ms) : 0;
    return (remaining_ms + 999u) / 1000u;
}

clock_headline_t clock_model__headline(const clock_model_t *model, uint64_t now_ms) {
    if (clock_stopwatch__in_use(&model->stopwatch)) {
        return (clock_headline_t){CLOCK_HEADLINE_STOPWATCH, clock_stopwatch__elapsed_ms(&model->stopwatch, now_ms) / 1000u};
    }
    const clock_timer_t *best = NULL;
    for (size_t i = 0; i < model->timer_count; ++i) {
        const clock_timer_t *timer = &model->timers[i];
        bool sooner = best != NULL && clock_timer__remaining_s(timer, now_ms) < clock_timer__remaining_s(best, now_ms);
        if (best == NULL || (timer->running && !best->running) || (timer->running == best->running && sooner)) best = timer;
    }
    if (best == NULL) return (clock_headline_t){CLOCK_HEADLINE_NONE, 0};
    return (clock_headline_t){CLOCK_HEADLINE_TIMER, clock_timer__remaining_s(best, now_ms)};
}

clock_event_t clock_model__tick(clock_model_t *model, uint64_t now_ms, int64_t now_s) {
    for (size_t i = 0; i < model->timer_count; ++i) {
        clock_timer_t *timer = &model->timers[i];
        if (!timer->running || now_ms < timer->deadline_ms) continue;
        clock_event_t event = {CLOCK_EVENT_TIMER, timer->duration_s};
        clock_model__remove_timer(model, i);
        return event;
    }
    for (size_t i = 0; i < model->alarm_count; ++i) {
        clock_alarm_t *alarm = &model->alarms[i];
        if (!alarm->enabled || now_s < alarm->fire_at_s) continue;
        alarm->enabled = false;
        return (clock_event_t){CLOCK_EVENT_ALARM, alarm->seconds_of_day};
    }
    return (clock_event_t){CLOCK_EVENT_NONE, 0};
}

bool clock_model__add_timer(clock_model_t *model, uint32_t duration_s, uint64_t now_ms) {
    if (duration_s == 0 || model->timer_count == CLOCK_MODEL__MAX_TIMERS) return false;
    model->timers[model->timer_count++] = (clock_timer_t){
        .duration_s = duration_s,
        .remaining_ms = duration_s * 1000u,
        .deadline_ms = now_ms + duration_s * 1000u,
        .running = true,
    };
    return true;
}

void clock_model__toggle_timer(clock_model_t *model, size_t index, uint64_t now_ms) {
    if (index >= model->timer_count) return;
    clock_timer_t *timer = &model->timers[index];
    if (timer->running) {
        timer->remaining_ms = now_ms < timer->deadline_ms ? (uint32_t)(timer->deadline_ms - now_ms) : 0;
    } else {
        timer->deadline_ms = now_ms + timer->remaining_ms;
    }
    timer->running = !timer->running;
}

void clock_model__remove_timer(clock_model_t *model, size_t index) {
    if (index >= model->timer_count) return;
    memmove(&model->timers[index], &model->timers[index + 1], (model->timer_count - index - 1) * sizeof(clock_timer_t));
    model->timer_count--;
}

bool clock_model__add_alarm(clock_model_t *model, uint32_t seconds_of_day, int64_t now_s) {
    if (model->alarm_count == CLOCK_MODEL__MAX_ALARMS) return false;
    clock_alarm_t *alarm = &model->alarms[model->alarm_count++];
    alarm->seconds_of_day = seconds_of_day;
    alarm->enabled = true;
    alarm->fire_at_s = clock_alarm__next_fire(alarm, now_s);
    return true;
}

void clock_model__toggle_alarm(clock_model_t *model, size_t index, int64_t now_s) {
    if (index >= model->alarm_count) return;
    clock_alarm_t *alarm = &model->alarms[index];
    alarm->enabled = !alarm->enabled;
    if (alarm->enabled) alarm->fire_at_s = clock_alarm__next_fire(alarm, now_s);
}

void clock_model__remove_alarm(clock_model_t *model, size_t index) {
    if (index >= model->alarm_count) return;
    memmove(&model->alarms[index], &model->alarms[index + 1], (model->alarm_count - index - 1) * sizeof(clock_alarm_t));
    model->alarm_count--;
}

uint32_t clock_stopwatch__elapsed_ms(const clock_stopwatch_t *stopwatch, uint64_t now_ms) {
    return stopwatch->running ? stopwatch->elapsed_ms + (uint32_t)(now_ms - stopwatch->started_ms)
                              : stopwatch->elapsed_ms;
}

void clock_stopwatch__toggle(clock_stopwatch_t *stopwatch, uint64_t now_ms) {
    if (stopwatch->running) stopwatch->elapsed_ms = clock_stopwatch__elapsed_ms(stopwatch, now_ms);
    else stopwatch->started_ms = now_ms;
    stopwatch->running = !stopwatch->running;
}

void clock_stopwatch__lap(clock_stopwatch_t *stopwatch, uint64_t now_ms) {
    if (!stopwatch->running) return;
    if (stopwatch->lap_count == CLOCK_MODEL__MAX_LAPS) {
        memmove(stopwatch->laps_ms, stopwatch->laps_ms + 1, (CLOCK_MODEL__MAX_LAPS - 1) * sizeof(uint32_t));
        stopwatch->lap_count--;
    }
    stopwatch->laps_ms[stopwatch->lap_count++] = clock_stopwatch__elapsed_ms(stopwatch, now_ms);
}

void clock_stopwatch__reset(clock_stopwatch_t *stopwatch) { memset(stopwatch, 0, sizeof(*stopwatch)); }
