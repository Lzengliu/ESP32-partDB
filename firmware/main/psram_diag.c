#include "psram_diag.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_private/esp_psram_extram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mmu_hal.h"
#include "sdkconfig.h"
#include "soc/soc.h"

static const char *TAG = "psram_diag";

static bool s_mem_test_ran;
static bool s_mem_test_ok;
static esp_err_t s_last_mem_test_err = ESP_ERR_INVALID_STATE;
static esp_err_t s_last_add_heap_err = ESP_ERR_INVALID_STATE;

#ifndef CONFIG_MMU_PAGE_SIZE
#define CONFIG_MMU_PAGE_SIZE 0x10000
#endif

static void find_psram_mapping(uintptr_t *out_start, uintptr_t *out_end)
{
    uintptr_t start = 0;
    uintptr_t end = 0;

    for (uintptr_t addr = SOC_EXTRAM_DATA_LOW;
         addr < SOC_EXTRAM_DATA_HIGH;
         addr += CONFIG_MMU_PAGE_SIZE) {
        if (esp_psram_check_ptr_addr((const void *)addr)) {
            if (start == 0) {
                start = addr;
            }
            end = addr + CONFIG_MMU_PAGE_SIZE;
        } else if (start != 0) {
            break;
        }
    }

    if (out_start) {
        *out_start = start;
    }
    if (out_end) {
        *out_end = end;
    }
}

void psram_diag_collect(psram_diag_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->initialized = esp_psram_is_initialized();
    out->chip_size = esp_psram_get_size();
    out->heap_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->heap_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    find_psram_mapping(&out->mapped_start, &out->mapped_end);
    if (out->mapped_end > out->mapped_start) {
        out->mapped_size = out->mapped_end - out->mapped_start;
    }
#if CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION
    out->heap_candidate_size = esp_psram_get_heap_size_to_protect();
#endif
    out->mem_test_ran = s_mem_test_ran;
    out->mem_test_ok = s_mem_test_ok;
    out->last_mem_test_err = s_last_mem_test_err;
    out->last_add_heap_err = s_last_add_heap_err;
}

void psram_diag_log_boot(void)
{
    psram_diag_status_t st;
    psram_diag_collect(&st);
    ESP_LOGI(TAG,
             "PSRAM init=%d chip=%u heap_total=%u heap_free=%u largest=%u mapped=0x%08x..0x%08x mapped_size=%u heap_candidate=%u",
             st.initialized,
             (unsigned)st.chip_size,
             (unsigned)st.heap_total,
             (unsigned)st.heap_free,
             (unsigned)st.heap_largest,
             (unsigned)st.mapped_start,
             (unsigned)st.mapped_end,
             (unsigned)st.mapped_size,
             (unsigned)st.heap_candidate_size);
}

esp_err_t psram_diag_run_read_probe(void)
{
    psram_diag_status_t st;
    psram_diag_collect(&st);
    if (!st.initialized || st.chip_size == 0 || st.mapped_size == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    enum { MAX_SAMPLES = 8 };
    size_t samples = 0;
    ESP_LOGW(TAG, "PSRAM read-only probe start mapped=0x%08x..0x%08x page_size=%u max_samples=%u",
             (unsigned)st.mapped_start,
             (unsigned)st.mapped_end,
             (unsigned)CONFIG_MMU_PAGE_SIZE,
             (unsigned)MAX_SAMPLES);

    for (uintptr_t addr = st.mapped_start;
         addr < st.mapped_end && samples < MAX_SAMPLES;
         addr += CONFIG_MMU_PAGE_SIZE) {
        if (!esp_psram_check_ptr_addr((const void *)addr)) {
            continue;
        }
        uint32_t paddr = 0;
        mmu_target_t target = 0;
        bool mapped = mmu_hal_vaddr_to_paddr(0, (uint32_t)addr, &paddr, &target);
        ESP_LOGW(TAG, "PSRAM read_probe[%u] before_read vaddr=0x%08x mmu_mapped=%d paddr=0x%08x target=0x%x",
                 (unsigned)samples,
                 (unsigned)addr,
                 mapped,
                 (unsigned)paddr,
                 (unsigned)target);
        volatile uint32_t *ptr = (volatile uint32_t *)addr;
        uint32_t got = *ptr;
        ESP_LOGW(TAG, "PSRAM read_probe[%u] read_ok value=0x%08x",
                 (unsigned)samples, (unsigned)got);
        samples++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return samples > 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t psram_diag_run_heap_probe(void)
{
    psram_diag_status_t st;
    psram_diag_collect(&st);
    if (!st.initialized || st.chip_size == 0 || st.heap_total == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = 512 * 1024;
    uint32_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        len = 128 * 1024;
        buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t words = len / sizeof(uint32_t);
    for (size_t i = 0; i < words; i++) {
        buf[i] = 0x5A000000u ^ (uint32_t)i ^ ((uint32_t)i << 11);
        if ((i & 0x0FFF) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    size_t first_bad = SIZE_MAX;
    uint32_t got_bad = 0;
    for (size_t i = 0; i < words; i++) {
        uint32_t expected = 0x5A000000u ^ (uint32_t)i ^ ((uint32_t)i << 11);
        uint32_t got = buf[i];
        if (got != expected) {
            first_bad = i;
            got_bad = got;
            break;
        }
        if ((i & 0x0FFF) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    heap_caps_free(buf);
    if (first_bad != SIZE_MAX) {
        ESP_LOGE(TAG, "PSRAM heap probe failed len=%u word=%u got=0x%08x",
                 (unsigned)len, (unsigned)first_bad, (unsigned)got_bad);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PSRAM heap probe passed len=%u", (unsigned)len);
    return ESP_OK;
}

esp_err_t psram_diag_run_mem_test(void)
{
    s_mem_test_ran = true;
    s_mem_test_ok = false;

    psram_diag_status_t st;
    psram_diag_collect(&st);
    if (!st.initialized || st.chip_size == 0 || st.mapped_size == 0) {
        s_last_mem_test_err = ESP_ERR_NOT_FOUND;
        return s_last_mem_test_err;
    }
    if (st.heap_total > 0) {
        s_last_mem_test_err = ESP_ERR_INVALID_STATE;
        return s_last_mem_test_err;
    }

    enum { MAX_SAMPLES = 8 };
    volatile uint32_t *ptrs[MAX_SAMPLES];
    uint32_t saved[MAX_SAMPLES];
    uint32_t patterns[MAX_SAMPLES];
    size_t samples = 0;

    ESP_LOGW(TAG, "PSRAM mapped access probe start mapped=0x%08x..0x%08x page_size=%u max_samples=%u",
             (unsigned)st.mapped_start,
             (unsigned)st.mapped_end,
             (unsigned)CONFIG_MMU_PAGE_SIZE,
             (unsigned)MAX_SAMPLES);

    for (uintptr_t addr = st.mapped_start;
         addr < st.mapped_end && samples < MAX_SAMPLES;
         addr += CONFIG_MMU_PAGE_SIZE) {
        if (!esp_psram_check_ptr_addr((const void *)addr)) {
            continue;
        }
        ptrs[samples] = (volatile uint32_t *)addr;
        ESP_LOGW(TAG, "PSRAM probe[%u] before_read addr=0x%08x",
                 (unsigned)samples, (unsigned)addr);
        saved[samples] = *ptrs[samples];
        ESP_LOGW(TAG, "PSRAM probe[%u] read_ok value=0x%08x",
                 (unsigned)samples, (unsigned)saved[samples]);
        patterns[samples] = 0xA5000000u ^ (uint32_t)addr ^ (uint32_t)(samples * 0x10204081u);
        ESP_LOGW(TAG, "PSRAM probe[%u] before_write pattern=0x%08x",
                 (unsigned)samples, (unsigned)patterns[samples]);
        *ptrs[samples] = patterns[samples];
        ESP_LOGW(TAG, "PSRAM probe[%u] write_ok", (unsigned)samples);
        samples++;
        if ((samples & 0x0F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    size_t first_bad = SIZE_MAX;
    uint32_t got_bad = 0;
    for (size_t i = 0; i < samples; i++) {
        ESP_LOGW(TAG, "PSRAM verify[%u] before_read addr=0x%08x",
                 (unsigned)i, (unsigned)(uintptr_t)ptrs[i]);
        uint32_t got = *ptrs[i];
        ESP_LOGW(TAG, "PSRAM verify[%u] read_ok value=0x%08x",
                 (unsigned)i, (unsigned)got);
        if (got != patterns[i] && first_bad == SIZE_MAX) {
            first_bad = i;
            got_bad = got;
        }
        if ((i & 0x0F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    for (size_t i = 0; i < samples; i++) {
        ESP_LOGW(TAG, "PSRAM restore[%u] before_write addr=0x%08x value=0x%08x",
                 (unsigned)i, (unsigned)(uintptr_t)ptrs[i], (unsigned)saved[i]);
        *ptrs[i] = saved[i];
        ESP_LOGW(TAG, "PSRAM restore[%u] write_ok", (unsigned)i);
        if ((i & 0x0F) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    bool ok = samples > 0 && first_bad == SIZE_MAX;
    s_mem_test_ok = ok;
    s_last_mem_test_err = ok ? ESP_OK : ESP_FAIL;
    if (ok) {
        ESP_LOGI(TAG, "PSRAM sampled memory test passed samples=%u page_size=%u",
                 (unsigned)samples, (unsigned)CONFIG_MMU_PAGE_SIZE);
    } else {
        ESP_LOGE(TAG, "PSRAM sampled memory test failed samples=%u first_bad=%u addr=0x%08x expected=0x%08x got=0x%08x",
                 (unsigned)samples,
                 (unsigned)first_bad,
                 first_bad == SIZE_MAX ? 0 : (unsigned)(uintptr_t)ptrs[first_bad],
                 first_bad == SIZE_MAX ? 0 : (unsigned)patterns[first_bad],
                 (unsigned)got_bad);
    }
    return s_last_mem_test_err;
}

esp_err_t psram_diag_add_to_heap(void)
{
    psram_diag_status_t st;
    psram_diag_collect(&st);
    if (!st.initialized || st.chip_size == 0 || st.mapped_size == 0) {
        s_last_add_heap_err = ESP_ERR_NOT_FOUND;
        return s_last_add_heap_err;
    }
    if (st.heap_total > 0) {
        s_last_add_heap_err = ESP_OK;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Manual PSRAM heap registration requested; mapped=0x%08x..0x%08x size=%u",
             (unsigned)st.mapped_start,
             (unsigned)st.mapped_end,
             (unsigned)st.mapped_size);
    s_last_add_heap_err = esp_psram_extram_add_to_heap_allocator();
    ESP_LOGI(TAG, "Manual PSRAM heap registration result=%s total=%u free=%u",
             esp_err_to_name(s_last_add_heap_err),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return s_last_add_heap_err;
}
