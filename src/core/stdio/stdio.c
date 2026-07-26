#include "core_sdk/stdio.h"

#include <stdbool.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: export
#include "freertos/task.h"

#include "core_sdk/result.h"

bruce_result_t bruce_stdio_read(void *buffer, size_t capacity, uint32_t timeout_ms, size_t *out_size)
{
    if (buffer == NULL || capacity == 0 || out_size == NULL) return BRUCE_ERR_INVALID_ARGUMENT;
    *out_size = 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval timeout = {
        .tv_sec = (time_t)(timeout_ms / 1000u),
        .tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u),
    };
    int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &timeout);
    if (ready == 0) return BRUCE_ERR_TIMEOUT;
    if (ready < 0) return BRUCE_ERR_IO;
    ssize_t count = read(STDIN_FILENO, buffer, capacity);
    if (count < 0) return BRUCE_ERR_IO;
    if (count == 0) return BRUCE_ERR_NOT_FOUND;
    *out_size = (size_t)count;
    return BRUCE_OK;
}

int bruce_stdio_read_line(char *buffer, size_t buffer_size, bool mask_input)
{
    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    size_t i = 0;
    bool eof = false;

    while (i + 1 < buffer_size) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

        int c = getchar();
        if (c == EOF) {
            if (ready > 0) {
                /* select reported data available, but read returned EOF. */
                eof = true;
                break;
            }
            /* No input available yet; keep waiting. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\b' || c == 0x7f) {
            if (i > 0) {
                i--;
                if (!mask_input) {
                    printf("\b \b");
                    fflush(stdout);
                }
            }
            continue;
        }

        buffer[i++] = (char)c;
        if (!mask_input) {
            putchar(c);
            fflush(stdout);
        }
    }

    buffer[i] = '\0';
    printf("\n");

    if (eof && i == 0) {
        return -1;
    }

    return (int)i;
}
