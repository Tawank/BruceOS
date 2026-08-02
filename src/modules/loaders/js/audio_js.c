#include "audio_js.h"

#include <stdbool.h>
#include <stdint.h>

#include "core_sdk/audio.h"

JSValue native_tone(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsNumber(ctx, argv[0]) || !JS_IsNumber(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "audio.tone(frequencyHz:int, durationMs:int, nonBlocking?:bool)");
    }

    int frequency = 0;
    int duration = 0;
    JS_ToInt32(ctx, &frequency, argv[0]);
    JS_ToInt32(ctx, &duration, argv[1]);
    bool non_blocking = argc > 2 && JS_ToBool(ctx, argv[2]);
    if (frequency < (int)BRUCE_AUDIO_MIN_FREQUENCY_HZ || frequency > (int)BRUCE_AUDIO_MAX_FREQUENCY_HZ ||
        duration <= 0 || duration > (int)BRUCE_AUDIO_MAX_TONE_DURATION_MS) {
        return JS_ThrowRangeError(ctx, "audio.tone arguments out of range");
    }

    bruce_result_t result = audio__tone((uint32_t)frequency, (uint32_t)duration, non_blocking);
    if (result != BRUCE_OK) return JS_ThrowInternalError(ctx, "audio.tone failed: %d", (int)result);
    return JS_UNDEFINED;
}
