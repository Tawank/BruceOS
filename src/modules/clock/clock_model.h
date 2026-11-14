#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOCK_MODEL__MAX_TIMERS 6
#define CLOCK_MODEL__MAX_ALARMS 6
#define CLOCK_MODEL__MAX_LAPS 8
#define CLOCK_MODEL__SECONDS_PER_DAY 86400

typedef struct {
    uint32_t duration_s;
    uint32_t remaining_ms;
    uint64_t deadline_ms;
    bool running;
} clock_timer_t;

typedef struct {
    uint32_t seconds_of_day;
    int64_t fire_at_s;
    bool enabled;
} clock_alarm_t;

typedef struct {
    uint64_t started_ms;
    uint32_t elapsed_ms;
    uint32_t laps_ms[CLOCK_MODEL__MAX_LAPS];
    size_t lap_count;
    bool running;
} clock_stopwatch_t;

typedef struct {
    clock_timer_t timers[CLOCK_MODEL__MAX_TIMERS];
    size_t timer_count;
    clock_alarm_t alarms[CLOCK_MODEL__MAX_ALARMS];
    size_t alarm_count;
    clock_stopwatch_t stopwatch;
} clock_model_t;

typedef enum {
    CLOCK_EVENT_NONE,
    CLOCK_EVENT_TIMER,
    CLOCK_EVENT_ALARM,
} clock_event_kind_t;

typedef struct {
    clock_event_kind_t kind;
    uint32_t seconds;
} clock_event_t;

/* Absolute local time in seconds, as returned by clock__datetime_to_epoch(). */
int64_t clock_alarm__next_fire(const clock_alarm_t *alarm, int64_t now_s);

bool clock_model__is_active(const clock_model_t *model);

/* Fires at most one due timer or alarm; fired timers are removed, fired alarms are disabled. */
clock_event_t clock_model__tick(clock_model_t *model, uint64_t now_ms, int64_t now_s);

bool clock_model__add_timer(clock_model_t *model, uint32_t duration_s, uint64_t now_ms);
void clock_model__toggle_timer(clock_model_t *model, size_t index, uint64_t now_ms);
void clock_model__remove_timer(clock_model_t *model, size_t index);
uint32_t clock_timer__remaining_s(const clock_timer_t *timer, uint64_t now_ms);

bool clock_model__add_alarm(clock_model_t *model, uint32_t seconds_of_day, int64_t now_s);
void clock_model__toggle_alarm(clock_model_t *model, size_t index, int64_t now_s);
void clock_model__remove_alarm(clock_model_t *model, size_t index);

void clock_stopwatch__toggle(clock_stopwatch_t *stopwatch, uint64_t now_ms);
void clock_stopwatch__lap(clock_stopwatch_t *stopwatch, uint64_t now_ms);
void clock_stopwatch__reset(clock_stopwatch_t *stopwatch);
uint32_t clock_stopwatch__elapsed_ms(const clock_stopwatch_t *stopwatch, uint64_t now_ms);
