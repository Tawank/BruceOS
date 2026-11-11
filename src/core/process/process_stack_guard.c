/*
 * Diagnostic for the "task stack is sane" cache asserts
 * (esp_cache_freeze_caches_disable_interrupts / spi_flash_disable_interrupts_
 * caches_and_other_cpu). ESP-IDF only reports the failed expression, and the
 * backtrace is useless because the stack pointer itself is bad, so these
 * --wrap shims (see src/CMakeLists.txt) print which task was running, where
 * its real stack is, and who started the flash/cache operation, then let the
 * original function assert as before.
 *
 * Both wrappers are IRAM_ATTR: the real functions return with the flash cache
 * disabled, so the code returning from them must not live in flash. The
 * reporter itself runs from flash, which is fine because it only runs before
 * the real function, while the cache is still enabled.
 */
#include <stdint.h>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

void __real_esp_cache_freeze_caches_disable_interrupts(void);
void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);

static inline bool IRAM_ATTR process_stack_guard__sp_is_sane(const void *sp) {
    return esp_ptr_in_dram(sp)
#if CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP
           || esp_ptr_in_rtc_dram_fast(sp)
#endif
        ;
}

static void __attribute__((noinline)) process_stack_guard__report(const char *operation, uint32_t sp, uint32_t caller) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    const char *name = task != NULL ? pcTaskGetName(task) : NULL;
    StackType_t *stack_start = task != NULL ? xTaskGetStackStart(task) : NULL;
    esp_rom_printf(
        "\n[stack_guard] %s with bad SP 0x%08x on core %d\n"
        "[stack_guard] task '%s' (handle %p), stack starts at %p\n"
        "[stack_guard] called from 0x%08x\n",
        operation,
        (unsigned)sp,
        (int)esp_cpu_get_core_id(),
        name != NULL ? name : "?",
        (void *)task,
        (void *)stack_start,
        (unsigned)caller
    );
    /* Free stack left at its lowest point; 0 means it overflowed. Only read
     * when the stack start looks like real RAM, so a corrupted TCB can't make
     * this walk random memory. */
    if (task != NULL && stack_start != NULL && process_stack_guard__sp_is_sane(stack_start)) {
        esp_rom_printf("[stack_guard] stack high-water mark: %u bytes free\n", (unsigned)uxTaskGetStackHighWaterMark(task));
    }
}

static inline uint32_t IRAM_ATTR process_stack_guard__caller(void *return_address) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    /* Xtensa keeps the call window size in the top two bits of a0. */
    return ((uint32_t)return_address & 0x3fffffffu) | 0x40000000u;
#else
    return (uint32_t)return_address;
#endif
}

void IRAM_ATTR __wrap_esp_cache_freeze_caches_disable_interrupts(void) {
    const void *sp = (const void *)esp_cpu_get_sp();
    if (!process_stack_guard__sp_is_sane(sp)) {
        process_stack_guard__report(
            "cache freeze", (uint32_t)sp, process_stack_guard__caller(__builtin_return_address(0))
        );
    }
    __real_esp_cache_freeze_caches_disable_interrupts();
}

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void) {
    const void *sp = (const void *)esp_cpu_get_sp();
    if (!process_stack_guard__sp_is_sane(sp)) {
        process_stack_guard__report(
            "flash op", (uint32_t)sp, process_stack_guard__caller(__builtin_return_address(0))
        );
    }
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
}
