#include "wifi_webui.h"

#include <stdio.h>

bool wifi__start_webui(bool ap_mode)
{
    printf("Web UI requested in %s mode\n", ap_mode ? "AP" : "STA");
    return true;
}
