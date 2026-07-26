#include "core_sdk/notification.h"

#include "core/display/display.h"

bruce_result_t notification__push(const char *text, uint32_t duration_ms)
{
    return display__notification_push(text, duration_ms);
}

bruce_result_t notification__dismiss(void)
{
    return display__notification_dismiss();
}
