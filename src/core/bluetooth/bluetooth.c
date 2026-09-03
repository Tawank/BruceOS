/* Backend-agnostic facade: dispatches to whichever BLE host stack is
 * compiled in (bluetooth_bluedroid.c or bluetooth_nimble.c -- selected in
 * src/CMakeLists.txt by CONFIG_BT_NIMBLE_ENABLED). Both backends implement
 * bluetooth__stack_init(), declared in bluetooth_internal.h. */

#include "core/bluetooth/bluetooth.h"

#include "core/bluetooth/bluetooth_internal.h"

bruce_result_t bluetooth__init(void) { return bluetooth__stack_init(); }
