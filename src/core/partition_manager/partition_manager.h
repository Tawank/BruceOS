#pragma once

#include <stdbool.h>

#include "core_sdk/partition_manager.h" // IWYU pragma: export

#include "esp_partition.h"

/* Core-private entry point, called exactly once by storage__init() before it
 * mounts anything. Reads (or synthesizes, on a device with no committed user
 * partition table) the effective partition layout for the user area and
 * registers every entry as an esp_partition_t via
 * esp_partition_register_external(), including applying any pending
 * format (see bruce_partition_entry_t.format_pending) left over from the
 * previous commit__() - this is the one point where a create/delete/format
 * actually takes effect on flash.
 *
 * *out_root_littlefs receives the registered partition labeled "littlefs"
 * (every layout, legacy or user-defined, always has exactly one) for
 * storage__init() to mount at STORAGE__MOUNT_PATH exactly as it does today.
 * *out_layout_owned reports whether the layout was already claimed by Bruce.
 */
bruce_result_t
partition_manager__init_for_storage(const esp_partition_t **out_root_littlefs, bool *out_layout_owned);

/* Persist the current default layout after its root LittleFS volume mounts.
 * This is deliberately deferred until after the successful mount so a foreign
 * firmware's data area remains identifiable for migration recovery. */
bruce_result_t partition_manager__claim_layout(void);
