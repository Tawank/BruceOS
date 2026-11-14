#include "clock_model_test.h"

#include "modules/clock/clock_model.h"

#include <stdio.h>
#include <string.h>

#define CLOCK_MODEL_TEST__CHECK(condition)                                                \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            printf("[selftest] clock/model: %s failed (line %d)\n", #condition, __LINE__); \
            return false;                                                                 \
        }                                                                                 \
    } while (0)

static bool clock_model_test__timers(void) {
    clock_model_t model;
    memset(&model, 0, sizeof(model));
    CLOCK_MODEL_TEST__CHECK(!clock_model__add_timer(&model, 0, 0));
    CLOCK_MODEL_TEST__CHECK(clock_model__add_timer(&model, 10, 1000));
    CLOCK_MODEL_TEST__CHECK(clock_model__add_timer(&model, 100, 1000));
    CLOCK_MODEL_TEST__CHECK(clock_model__is_active(&model));
    CLOCK_MODEL_TEST__CHECK(clock_timer__remaining_s(&model.timers[0], 4500) == 7);

    clock_model__toggle_timer(&model, 0, 4500);
    CLOCK_MODEL_TEST__CHECK(clock_timer__remaining_s(&model.timers[0], 60000) == 7);
    clock_model__toggle_timer(&model, 0, 60000);
    CLOCK_MODEL_TEST__CHECK(clock_timer__remaining_s(&model.timers[0], 62000) == 5);

    clock_event_t event = clock_model__tick(&model, 66499, 0);
    CLOCK_MODEL_TEST__CHECK(event.kind == CLOCK_EVENT_NONE);
    event = clock_model__tick(&model, 66500, 0);
    CLOCK_MODEL_TEST__CHECK(event.kind == CLOCK_EVENT_TIMER && event.seconds == 10);
    CLOCK_MODEL_TEST__CHECK(model.timer_count == 1);
    event = clock_model__tick(&model, 101000, 0);
    CLOCK_MODEL_TEST__CHECK(event.kind == CLOCK_EVENT_TIMER && event.seconds == 100 && model.timer_count == 0);
    CLOCK_MODEL_TEST__CHECK(!clock_model__is_active(&model));

    for (int i = 0; i < CLOCK_MODEL__MAX_TIMERS; ++i) CLOCK_MODEL_TEST__CHECK(clock_model__add_timer(&model, 1, 0));
    CLOCK_MODEL_TEST__CHECK(!clock_model__add_timer(&model, 1, 0));
    return true;
}

static bool clock_model_test__alarms(void) {
    const int64_t midnight = 20 * (int64_t)CLOCK_MODEL__SECONDS_PER_DAY;
    clock_model_t model;
    memset(&model, 0, sizeof(model));

    CLOCK_MODEL_TEST__CHECK(clock_model__add_alarm(&model, 7 * 3600, midnight + 8 * 3600));
    CLOCK_MODEL_TEST__CHECK(model.alarms[0].fire_at_s == midnight + CLOCK_MODEL__SECONDS_PER_DAY + 7 * 3600);
    CLOCK_MODEL_TEST__CHECK(clock_model__add_alarm(&model, 9 * 3600, midnight + 8 * 3600));
    CLOCK_MODEL_TEST__CHECK(model.alarms[1].fire_at_s == midnight + 9 * 3600);

    clock_event_t event = clock_model__tick(&model, 0, midnight + 9 * 3600 - 1);
    CLOCK_MODEL_TEST__CHECK(event.kind == CLOCK_EVENT_NONE);
    event = clock_model__tick(&model, 0, midnight + 9 * 3600 + 30);
    CLOCK_MODEL_TEST__CHECK(event.kind == CLOCK_EVENT_ALARM && event.seconds == 9 * 3600 && !model.alarms[1].enabled);
    event = clock_model__tick(&model, 0, midnight + 9 * 3600 + 31);
    CLOCK_MODEL_TEST__CHECK(event.kind == CLOCK_EVENT_NONE);

    clock_model__toggle_alarm(&model, 1, midnight + 9 * 3600 + 31);
    CLOCK_MODEL_TEST__CHECK(model.alarms[1].enabled && model.alarms[1].fire_at_s > midnight + CLOCK_MODEL__SECONDS_PER_DAY);
    clock_model__remove_alarm(&model, 0);
    CLOCK_MODEL_TEST__CHECK(model.alarm_count == 1 && model.alarms[0].seconds_of_day == 9 * 3600);
    return true;
}

static bool clock_model_test__stopwatch(void) {
    clock_stopwatch_t stopwatch;
    memset(&stopwatch, 0, sizeof(stopwatch));
    clock_stopwatch__lap(&stopwatch, 100);
    CLOCK_MODEL_TEST__CHECK(stopwatch.lap_count == 0);

    clock_stopwatch__toggle(&stopwatch, 1000);
    CLOCK_MODEL_TEST__CHECK(clock_stopwatch__elapsed_ms(&stopwatch, 1500) == 500);
    clock_stopwatch__lap(&stopwatch, 1500);
    clock_stopwatch__toggle(&stopwatch, 2000);
    CLOCK_MODEL_TEST__CHECK(clock_stopwatch__elapsed_ms(&stopwatch, 9000) == 1000);
    clock_stopwatch__toggle(&stopwatch, 10000);
    CLOCK_MODEL_TEST__CHECK(clock_stopwatch__elapsed_ms(&stopwatch, 10500) == 1500);

    for (int i = 0; i < CLOCK_MODEL__MAX_LAPS + 3; ++i) clock_stopwatch__lap(&stopwatch, 10500 + (uint64_t)i);
    CLOCK_MODEL_TEST__CHECK(stopwatch.lap_count == CLOCK_MODEL__MAX_LAPS);
    clock_stopwatch__reset(&stopwatch);
    CLOCK_MODEL_TEST__CHECK(!stopwatch.running && stopwatch.lap_count == 0 && clock_stopwatch__elapsed_ms(&stopwatch, 99999) == 0);
    return true;
}

bool selftest__run_clock_model_case(void) {
    return clock_model_test__timers() && clock_model_test__alarms() && clock_model_test__stopwatch();
}
