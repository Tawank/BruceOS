/* What a library in /lib may import. tools/build_libs.py fails the build when
 * a library needs a name missing here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_elf.h"
#include "esp_log.h"
#include "private/elf_symbol.h"

/* picolibc's getreent.c: newlib-style code reaches stdio's FILEs through it.
 * Parenthesised, since <sys/reent.h> also has a __getreent() macro. */
void *(__getreent)(void);
/* picolibc's errno.c, for newlib-style errno. */
int *(__errno)(void);

/* C++ operator new/delete for libraries written in C++ (ALAC). */
static void *lib__operator_new(size_t size) { return malloc(size); }

static void lib__operator_delete(void *pointer, size_t size) {
    (void)size;
    free(pointer);
}

const struct esp_elfsym g_bruce_lib_elfsyms[] = {
    {"_Znwj",   (void *)lib__operator_new   },
    {"_Znaj",   (void *)lib__operator_new   },
    {"_ZdlPvj", (void *)lib__operator_delete},
    {"_ZdlPv",  (void *)lib__operator_delete},
    {"_ZdaPv",  (void *)lib__operator_delete},
    ESP_ELFSYM_EXPORT(malloc),
    ESP_ELFSYM_EXPORT(calloc),
    ESP_ELFSYM_EXPORT(realloc),
    ESP_ELFSYM_EXPORT(free),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(memchr),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strchr),
    ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(__getreent),
    ESP_ELFSYM_EXPORT(__errno),
    ESP_ELFSYM_EXPORT(puts),
    ESP_ELFSYM_EXPORT(fprintf),
    ESP_ELFSYM_EXPORT(fwrite),
    ESP_ELFSYM_EXPORT(fclose),
    ESP_ELFSYM_EXPORT(printf),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(vsnprintf),
    ESP_ELFSYM_EXPORT(qsort),
    ESP_ELFSYM_EXPORT(abort),
    ESP_ELFSYM_EXPORT(esp_log_timestamp),
    ESP_ELFSYM_EXPORT(esp_log_write),
    ESP_ELFSYM_EXPORT(esp_log_writev),
    ESP_ELFSYM_END,
};
