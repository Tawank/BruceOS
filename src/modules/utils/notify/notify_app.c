#include "notify_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "core_sdk/notification.h"
#include "core_sdk/status_icon.h"
#include "core_sdk/stdio.h"

int notify_app_main(int argc, char **argv) {
    ArgParser *root = ap_new_parser();
    if (root == NULL) return BRUCE_ERR_NO_MEMORY;
    ap_set_helptext(root, "Manage transient notifications and status icons.");
    ArgParser *push = ap_new_cmd(root, "push");
    ArgParser *dismiss = ap_new_cmd(root, "dismiss");
    ArgParser *icon_list = ap_new_cmd(root, "icon-list");
    ArgParser *icon_remove = ap_new_cmd(root, "icon-remove");
    if (push != NULL) {
        ap_set_helptext(push, "Push a notification: push <duration-ms> <text...>");
        ap_add_required_arg(push, "duration-ms", "Display duration in milliseconds");
        ap_allow_extra_args(push);
        ap_first_pos_arg_ends_option_parsing(push);
    }
    if (dismiss != NULL) {
        ap_set_helptext(dismiss, "Dismiss the current notification.");
    }
    if (icon_list != NULL) {
        ap_set_helptext(icon_list, "List active status icons.");
    }
    if (icon_remove != NULL) {
        ap_set_helptext(icon_remove, "Remove a status icon by key.");
        ap_add_required_arg(icon_remove, "key", "Status icon key");
        ap_unknown_options_as_args(icon_remove);
    }

    if (argc < 1 || !ap_parse(root, argc, argv)) {
        ap_status_t status = ap_get_status(root);
        ap_free(root);
        if (status == AP_STATUS_HELP || status == AP_STATUS_VERSION) return BRUCE_OK;
        return status == AP_STATUS_NO_MEMORY ? BRUCE_ERR_NO_MEMORY : BRUCE_ERR_INVALID_ARGUMENT;
    }

    ArgParser *command = ap_get_cmd_parser(root);
    if (command == push) {
        if (ap_count_args(push) < 2) {
            ap_print_help(push);
            ap_free(root);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        char *end = NULL;
        char *duration_text = ap_get_arg_at_index(push, 0);
        unsigned long duration = strtoul(duration_text, &end, 10);
        if (end == duration_text || *end != '\0' || duration > UINT32_MAX) {
            ap_print_help(push);
            ap_free(root);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        size_t length = 0;
        for (int i = 1; i < ap_count_args(push); ++i) {
            length += strlen(ap_get_arg_at_index(push, i)) + (i > 1 ? 1u : 0u);
        }
        if (length >= BRUCE_NOTIFICATION_TEXT_MAX) {
            ap_free(root);
            return BRUCE_ERR_INVALID_ARGUMENT;
        }
        char text[BRUCE_NOTIFICATION_TEXT_MAX] = {0};
        for (int i = 1; i < ap_count_args(push); ++i) {
            if (i > 1) strcat(text, " ");
            strcat(text, ap_get_arg_at_index(push, i));
        }
        int result = notification__push(text, (uint32_t)duration);
        ap_free(root);
        return result;
    }
    if (command == dismiss) {
        int result = notification__dismiss();
        ap_free(root);
        return result;
    }
    if (command == icon_remove) {
        int result = status_icon__remove(ap_get_arg(icon_remove, "key"));
        ap_free(root);
        return result;
    }
    if (command == icon_list) {
        bruce_status_icon_t icons[BRUCE_STATUS_ICON_MAX];
        size_t count = 0;
        uint32_t revision = 0;
        bruce_result_t result = status_icon__list(icons, BRUCE_STATUS_ICON_MAX, &count, &revision);
        if (result != BRUCE_OK) {
            ap_free(root);
            return result;
        }
        stdio__printf("revision %lu, %u icon(s)\n", (unsigned long)revision, (unsigned)count);
        for (size_t i = 0; i < count; ++i) {
            stdio__printf("%s %ux%u\n", icons[i].key, icons[i].width, icons[i].height);
        }
        ap_free(root);
        return BRUCE_OK;
    }
    ap_print_help(root);
    ap_free(root);
    return BRUCE_ERR_INVALID_ARGUMENT;
}
