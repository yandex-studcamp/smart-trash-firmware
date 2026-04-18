#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "esp_err.h"

static const char *TAG = "spec";

static void print_chip_specs(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_err_t flash_err = esp_flash_get_size(NULL, &flash_size);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    ESP_LOGI(TAG, "========== ESP32-CAM SPEC ==========");
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Target: %s", CONFIG_IDF_TARGET);

    ESP_LOGI(TAG, "Chip cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Chip revision: %d", chip_info.revision);
    ESP_LOGI(TAG, "CPU freq: %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);

    ESP_LOGI(TAG, "Features:%s%s%s%s",
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? " WiFi" : "",
             (chip_info.features & CHIP_FEATURE_BT) ? " BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? " BLE" : "",
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? " EmbeddedFlash" : "");

    if (flash_err == ESP_OK) {
        ESP_LOGI(TAG, "Flash size: %" PRIu32 " bytes (%.2f MB)",
                 flash_size, flash_size / (1024.0 * 1024.0));
    } else {
        ESP_LOGW(TAG, "Flash size detect failed: %s", esp_err_to_name(flash_err));
    }

    ESP_LOGI(TAG, "Heap free (8-bit): %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Heap largest block (8-bit): %u bytes",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Internal heap free: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

#if CONFIG_SPIRAM
    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM size: %u bytes (%.2f MB)",
             (unsigned)psram_size, psram_size / (1024.0 * 1024.0));
    ESP_LOGI(TAG, "SPIRAM heap free: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
    ESP_LOGI(TAG, "PSRAM: disabled in sdkconfig (CONFIG_SPIRAM=n)");
#endif

    ESP_LOGI(TAG, "MAC STA: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "------- Partition Table -------");

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY,
        ESP_PARTITION_SUBTYPE_ANY,
        NULL
    );

    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        if (part) {
            ESP_LOGI(TAG,
                     "label=%s type=%d subtype=0x%02x addr=0x%08" PRIx32 " size=%" PRIu32 " (%.1f KB)",
                     part->label,
                     part->type,
                     part->subtype,
                     part->address,
                     part->size,
                     part->size / 1024.0);
        }
        it = esp_partition_next(it);
    }

    ESP_LOGI(TAG, "================================");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    print_chip_specs();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
