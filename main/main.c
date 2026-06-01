#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ads1298.h"
#include "wifi_manager.h"
#include "ads1298_stream.h"
#include "lsm9ds1.h"
#include "imu_stream.h"
#include "time_sync.h"
#include "pmic_npm1300.h"

static const char *TAG = "main";

static void log_memory_snapshot(const char *stage)
{
    ESP_LOGI(TAG,
             "MEM[%s] free=%uB min=%uB internal=%uB internal_min=%uB",
             stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    ESP_LOGI(TAG,
             "STACK[%s] main=%uB",
             stage,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
}

void app_main(void)
{
    log_memory_snapshot("boot");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    log_memory_snapshot("nvs_ready");

    esp_err_t pmic_ret = pmic_npm1300_init();
    if (pmic_ret != ESP_OK) {
        ESP_LOGW(TAG, "PMIC init failed (%s) – continuing without PMIC", esp_err_to_name(pmic_ret));
    }
    log_memory_snapshot("pmic_ready");

    wifi_manager_init();
    wifi_manager_wait_connected();
    log_memory_snapshot("wifi_connected");

    time_sync_start();
    /* Wait for time sync lock, or fall through after 5 s to start streams anyway */
    for (int i = 0; i < 50; i++) {
        if (time_sync_is_locked()) {
            ESP_LOGI(TAG, "time sync locked (offset=%lld µs)",
                     (long long)time_sync_offset_us());
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!time_sync_is_locked()) {
        ESP_LOGW(TAG, "time sync not locked after 5 s — proceeding with raw MCU time");
    }
    log_memory_snapshot("time_sync_ready");

    ESP_LOGI(TAG, "Initialising ADS1298...");
    ESP_ERROR_CHECK(ads1298_init());
    ESP_ERROR_CHECK(ads1298_start_conversion());
    ESP_LOGI(TAG, "ADS1298 running at 2000 SPS");
    log_memory_snapshot("ads1298_ready");

    ads1298_stream_init();
    ads1298_stream_start();
    log_memory_snapshot("emg_stream_started");

    ESP_LOGI(TAG, "Initialising LSM9DS1TR IMU...");
    ret = lsm9ds1_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LSM9DS1TR ready");
        imu_stream_init();
        imu_stream_start();
        log_memory_snapshot("imu_stream_started");
        ESP_LOGI(TAG, "System ready  EMG TCP:3333  IMU TCP:3334");
    } else {
        ESP_LOGW(TAG, "LSM9DS1TR not found (%s) – IMU stream disabled, EMG only",
                 esp_err_to_name(ret));
        log_memory_snapshot("imu_disabled");
        ESP_LOGI(TAG, "System ready  EMG TCP:3333  (IMU not present)");
    }
    while (1) {
        log_memory_snapshot("heartbeat");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
