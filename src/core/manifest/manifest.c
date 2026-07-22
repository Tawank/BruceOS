#include "core_sdk/manifest.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "core_sdk/permission.h"

#define MANIFEST__STACK_MIN 4096u
#define MANIFEST__STACK_MAX 16384u

static int manifest__base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes standard (RFC 4648) base64 with '=' padding into `out`, which must
 * be exactly `out_size` bytes.  Returns false on malformed input or a
 * decoded length that does not exactly match out_size. */
static bool manifest__base64_decode_exact(const char *in, uint8_t *out, size_t out_size)
{
    size_t in_len = strlen(in);
    if (in_len == 0 || in_len % 4 != 0) {
        return false;
    }

    size_t pad = 0;
    if (in[in_len - 1] == '=') pad++;
    if (in_len >= 2 && in[in_len - 2] == '=') pad++;

    size_t decoded_len = (in_len / 4) * 3 - pad;
    if (decoded_len != out_size) {
        return false;
    }

    size_t out_index = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        char c2 = in[i + 2];
        char c3 = in[i + 3];
        int v0 = manifest__base64_value(in[i]);
        int v1 = manifest__base64_value(in[i + 1]);
        int v2 = (c2 == '=') ? 0 : manifest__base64_value(c2);
        int v3 = (c3 == '=') ? 0 : manifest__base64_value(c3);
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            return false;
        }

        uint32_t triple = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) | ((uint32_t)v2 << 6) | (uint32_t)v3;
        if (out_index < out_size) out[out_index++] = (uint8_t)(triple >> 16);
        if (c2 != '=' && out_index < out_size) out[out_index++] = (uint8_t)(triple >> 8);
        if (c3 != '=' && out_index < out_size) out[out_index++] = (uint8_t)triple;
    }
    return out_index == out_size;
}

bruce_result_t manifest__parse(const char *json, size_t json_len, bruce_manifest_t *out_manifest)
{
    if (json == NULL || out_manifest == NULL || json_len == 0) {
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    memset(out_manifest, 0, sizeof(*out_manifest));

    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "appName");
    const cJSON *icon = cJSON_GetObjectItemCaseSensitive(root, "appIcon");
    const cJSON *abi = cJSON_GetObjectItemCaseSensitive(root, "coreAbiVersion");
    const cJSON *stack = cJSON_GetObjectItemCaseSensitive(root, "stackSize");
    const cJSON *permissions = cJSON_GetObjectItemCaseSensitive(root, "permissions");

    bool ok = cJSON_IsString(name) && name->valuestring != NULL && name->valuestring[0] != '\0' &&
              strlen(name->valuestring) < BRUCE_MANIFEST_APP_NAME_MAX;
    ok = ok && cJSON_IsString(icon) && icon->valuestring != NULL;
    ok = ok && cJSON_IsNumber(abi);
    ok = ok && cJSON_IsNumber(stack) && stack->valuedouble >= MANIFEST__STACK_MIN &&
        stack->valuedouble <= MANIFEST__STACK_MAX;
    ok = ok && (permissions == NULL || cJSON_IsArray(permissions));
    if (!ok) {
        cJSON_Delete(root);
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    if (!manifest__base64_decode_exact(icon->valuestring, out_manifest->app_icon, BRUCE_MANIFEST_ICON_BYTES)) {
        cJSON_Delete(root);
        return BRUCE_ERR_MANIFEST_INVALID;
    }

    strncpy(out_manifest->app_name, name->valuestring, BRUCE_MANIFEST_APP_NAME_MAX - 1);
    out_manifest->core_abi_version = (uint32_t)abi->valuedouble;
    out_manifest->stack_size = (uint32_t)stack->valuedouble;

    size_t permission_count = 0;
    if (permissions != NULL) {
        int array_size = cJSON_GetArraySize(permissions);
        if (array_size < 0 || (size_t)array_size > BRUCE_MANIFEST_MAX_PERMISSIONS) {
            cJSON_Delete(root);
            return BRUCE_ERR_MANIFEST_INVALID;
        }
        for (int i = 0; i < array_size; ++i) {
            const cJSON *entry = cJSON_GetArrayItem(permissions, i);
            bruce_permission_t permission;
            if (!cJSON_IsString(entry) || entry->valuestring == NULL ||
                !permission__from_name(entry->valuestring, &permission)) {
                cJSON_Delete(root);
                return BRUCE_ERR_MANIFEST_INVALID;
            }
            bool duplicate = false;
            for (size_t j = 0; j < permission_count; ++j) {
                if (strcmp(out_manifest->permissions[j], entry->valuestring) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate || strlen(entry->valuestring) >= BRUCE_MANIFEST_PERMISSION_NAME_MAX) {
                cJSON_Delete(root);
                return BRUCE_ERR_MANIFEST_INVALID;
            }
            strncpy(out_manifest->permissions[permission_count], entry->valuestring,
                    BRUCE_MANIFEST_PERMISSION_NAME_MAX - 1);
            permission_count++;
        }
    }
    out_manifest->permission_count = permission_count;

    cJSON_Delete(root);
    return BRUCE_OK;
}
