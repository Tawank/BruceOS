#pragma once

#include <stdio.h>

#include "core_sdk/stdio.h"

/* Picolibc stdout cannot be replaced per process, so self-test reports must use
 * the routed Core stdio API to remain visible in the GUI terminal. */
#define printf stdio__printf
