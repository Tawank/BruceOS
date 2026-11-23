/* For boards without a boards/<id>/board.c yet: no HAL devices, so lookups
 * find nothing and power requests do nothing. */

#include "hal/hal.h"

const hal_board_t hal_board = {.name = "generic"};
