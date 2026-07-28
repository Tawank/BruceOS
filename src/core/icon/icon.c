#include "core_sdk/icon.h"

#include <stdbool.h>
#include <string.h>

/*
 * Built-in icon paths are 24x24 SVG path-data strings from the Material Design
 * Icons project (https://pictogrammers.com/library/mdi/), used under the
 * Pictogrammers Free License.
 */

/* mdi-wifi */
static const char s_icon_wifi[] =
    "M12,21L15.6,16.2C14.6,15.45 13.35,15 12,15C10.65,15 9.4,15.45 8.4,16.2L12,21M12,3C7.95,3 "
    "4.21,4.34 1.2,6.6L3,9C5.5,7.12 8.62,6 12,6C15.38,6 18.5,7.12 21,9L22.8,6.6C19.79,4.34 16.05,3 "
    "12,3M12,9C9.3,9 6.81,9.89 4.8,11.4L6.6,13.8C8.1,12.67 9.97,12 12,12C14.03,12 15.9,12.67 "
    "17.4,13.8L19.2,11.4C17.19,9.89 14.7,9 12,9Z";

/* mdi-bluetooth */
static const char s_icon_bluetooth[] =
    "M14.88,16.29L13,18.17V14.41M13,5.83L14.88,7.71L13,9.58M17.71,7.71L12,2H11V9.58L6.41,5L5,6.41L10.59,12L5,"
    "17.58L6.41,19L11,14.41V22H12L17.71,16.29L13.41,12L17.71,7.71Z";

/* mdi-remote */
static const char s_icon_ir[] =
    "M12,0C8.96,0 6.21,1.23 4.22,3.22L5.63,4.63C7.26,3 9.5,2 12,2C14.5,2 16.74,3 "
    "18.36,4.64L19.77,3.23C17.79,1.23 15.04,0 12,0M7.05,6.05L8.46,7.46C9.37,6.56 10.62,6 12,6C13.38,6 "
    "14.63,6.56 15.54,7.46L16.95,6.05C15.68,4.78 13.93,4 12,4C10.07,4 8.32,4.78 7.05,6.05M12,15A2,2 0 0,1 "
    "10,13A2,2 0 0,1 12,11A2,2 0 0,1 14,13A2,2 0 0,1 12,15M15,9H9A1,1 0 0,0 8,10V22A1,1 0 0,0 9,23H15A1,1 0 "
    "0,0 16,22V10A1,1 0 0,0 15,9Z";

/* mdi-radio-handheld */
static const char s_icon_handheld[] =
    "M9,2A1,1 0 0,0 8,3C8,8.67 8,14.33 8,20C8,21.11 8.89,22 10,22H15C16.11,22 17,21.11 17,20V9C17,7.89 "
    "16.11,7 15,7H10V3A1,1 0 0,0 9,2M10,9H15V13H10V9Z";

/* mdi-folder */
static const char s_icon_folder[] =
    "M10,4H4C2.89,4 2,4.89 2,6V18A2,2 0 0,0 4,20H20A2,2 0 0,0 22,18V8C22,6.89 21.1,6 20,6H12L10,4Z";

/* mdi-console */
static const char s_icon_terminal[] =
    "M20,19V7H4V19H20M20,3A2,2 0 0,1 22,5V19A2,2 0 0,1 20,21H4A2,2 0 0,1 2,19V5C2,3.89 2.9,3 4,3H20"
    "M13,17V15H18V17H13M9.58,13L5.57,9H8.4L11.7,12.3C12.09,12.69 12.09,13.33 11.7,13.72L8.42,17H5.59"
    "L9.58,13Z";

/* mdi-clock */
static const char s_icon_clock[] =
    "M12,2A10,10 0 0,0 2,12A10,10 0 0,0 12,22A10,10 0 0,0 22,12A10,10 0 0,0 12,2"
    "M16.2,16.2L11,13V7H12.5V12.2L17,14.9L16.2,16.2Z";

/* mdi-cog */
static const char s_icon_settings[] =
    "M12,15.5A3.5,3.5 0 0,1 8.5,12A3.5,3.5 0 0,1 12,8.5A3.5,3.5 0 0,1 15.5,12A3.5,3.5 0 0,1 12,15.5"
    "M19.43,12.97C19.47,12.65 19.5,12.33 19.5,12C19.5,11.67 19.47,11.34 19.43,11L21.54,9.37"
    "C21.73,9.22 21.78,8.95 21.66,8.73L19.66,5.27C19.54,5.05 19.27,4.96 19.05,5.05L16.56,6.05"
    "C16.04,5.66 15.5,5.32 14.87,5.07L14.5,2.42C14.46,2.18 14.25,2 14,2H10C9.75,2 9.54,2.18 9.5,2.42"
    "L9.13,5.07C8.5,5.32 7.96,5.66 7.44,6.05L4.95,5.05C4.73,4.96 4.46,5.05 4.34,5.27L2.34,8.73"
    "C2.21,8.95 2.27,9.22 2.46,9.37L4.57,11C4.53,11.34 4.5,11.67 4.5,12C4.5,12.33 4.53,12.65 4.57,12.97"
    "L2.46,14.63C2.27,14.78 2.21,15.05 2.34,15.27L4.34,18.73C4.46,18.95 4.73,19.03 4.95,18.95L7.44,17.94"
    "C7.96,18.34 8.5,18.68 9.13,18.93L9.5,21.58C9.54,21.82 9.75,22 10,22H14C14.25,22 14.46,21.82 14.5,21.58"
    "L14.87,18.93C15.5,18.67 16.04,18.34 16.56,17.94L19.05,18.95C19.27,19.03 19.54,18.95 19.66,18.73"
    "L21.66,15.27C21.78,15.05 21.73,14.78 21.54,14.63L19.43,12.97Z";

/* mdi-test-tube */
static const char s_icon_selftest[] =
    "M7,2V4H8V18A4,4 0 0,0 12,22A4,4 0 0,0 16,18V4H17V2H7M11,16C10.4,16 10,15.6 10,15C10,14.4 10.4,14 "
    "11,14C11.6,14 12,14.4 12,15C12,15.6 11.6,16 11,16M13,12C12.4,12 12,11.6 12,11C12,10.4 12.4,10 13,10"
    "C13.6,10 14,10.4 14,11C14,11.6 13.6,12 13,12M14,7H10V4H14V7Z";

/* mdi-apps */
static const char s_icon_apps[] =
    "M16,20H20V16H16M16,14H20V10H16M10,8H14V4H10M16,8H20V4H16M10,14H14V10H10M4,14H8V10H4"
    "M4,20H8V16H4M10,20H14V16H10M4,8H8V4H4V8Z";

typedef struct {
    const char *name;
    const char *path;
} icon__entry_t;

static const icon__entry_t s_icons[] = {
    {"wifi",     s_icon_wifi     },
    {"ble",      s_icon_bluetooth},
    {"remote",   s_icon_ir       },
    {"handheld", s_icon_handheld },
    {"folder",   s_icon_folder   },
    {"files",    s_icon_folder   },
    {"terminal", s_icon_terminal },
    {"clock",    s_icon_clock    },
    {"settings", s_icon_settings },
    {"selftest", s_icon_selftest },
    {"apps",     s_icon_apps     },
};

const char *icon__get(const char *name) {
    if (name == NULL) { return NULL; }
    for (size_t i = 0; i < (sizeof(s_icons) / sizeof(s_icons[0])); ++i) {
        if (strcmp(name, s_icons[i].name) == 0) { return s_icons[i].path; }
    }
    return NULL;
}
