#include "notification_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_sdk/notification.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/stdio.h"

static int notification_app__usage(void) {
    stdio__printf(
        "usage: notification push <duration-ms> <text> | dismiss | icon-list | icon-remove <key>\n"
    );
    return BRUCE_ERR_INVALID_ARGUMENT;
}

int notification_app_main(int argc, char **argv) {
    if (argc < 1) return notification_app__usage();
    if (strcmp(argv[0], "push") == 0) {
        if (argc < 3) return notification_app__usage();
        char *end = NULL;
        unsigned long duration = strtoul(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || duration > UINT32_MAX) return notification_app__usage();
        size_t length = 0;
        for (int i = 2; i < argc; ++i) length += strlen(argv[i]) + (i > 2 ? 1u : 0u);
        if (length >= BRUCE_NOTIFICATION_TEXT_MAX) return BRUCE_ERR_INVALID_ARGUMENT;
        char text[BRUCE_NOTIFICATION_TEXT_MAX] = {0};
        for (int i = 2; i < argc; ++i) {
            if (i > 2) strcat(text, " ");
            strcat(text, argv[i]);
        }
        return notification__push(text, (uint32_t)duration);
    }
    if (strcmp(argv[0], "dismiss") == 0) { return notification__dismiss(); }
    if (strcmp(argv[0], "icon-remove") == 0) {
        return argc == 2 ? status_icon__remove(argv[1]) : notification_app__usage();
    }
    if (strcmp(argv[0], "icon-list") == 0) {
        bruce_status_icon_t icons[BRUCE_STATUS_ICON_MAX];
        size_t count = 0;
        uint32_t revision = 0;
        bruce_result_t result = status_icon__list(icons, BRUCE_STATUS_ICON_MAX, &count, &revision);
        if (result != BRUCE_OK) return result;
        stdio__printf("revision %lu, %u icon(s)\n", (unsigned long)revision, (unsigned)count);
        for (size_t i = 0; i < count; ++i) {
            stdio__printf("%s %ux%u\n", icons[i].key, icons[i].width, icons[i].height);
        }
        return BRUCE_OK;
    }
    return notification_app__usage();
}
