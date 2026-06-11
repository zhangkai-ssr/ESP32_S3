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
#include "led_ctrl.h"
#include "npm1300_led.h"   /* for npm1300_enable_sensor_rails() */

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

    led_ctrl_init();
    log_memory_snapshot("led_ready");

    /* Enable BUCK1/LDO1/LDO2 BEFORE talking to sensors. NPM1300 default
     * leaves these rails OFF; without this call ADS1298 has no AVDD and
     * IMU has no power. Must happen AFTER led_ctrl_init() (which installs
     * the I2C bus driver). Failure here is non-fatal — we still let the
     * board boot WiFi so we can SSH-debug. */
    if (npm1300_enable_sensor_rails() != ESP_OK) {
        ESP_LOGE(TAG, "sensor rails enable FAILED — sensors will not respond");
    }
    log_memory_snapshot("rails_enabled");

    wifi_manager_init();
    wifi_manager_wait_connected();
    log_memory_snapshot("wifi_connected");

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
    uint32_t last_isr = 0;
    while (1) {
        log_memory_snapshot("heartbeat");
        /* Diagnostic: every 5s print DRDY counters so we can see if the
         * ADS1298 is still producing samples even when TCP is down. */
        uint32_t isr = 0, take = 0, ovf = 0;
        ads1298_drdy_stats(&isr, &take, &ovf);
        ESP_LOGI(TAG, "DRDY: isr=%lu (+%lu in 5s) take=%lu overflow=%lu",
                 (unsigned long)isr,
                 (unsigned long)(isr - last_isr),
                 (unsigned long)take,
                 (unsigned long)ovf);
        last_isr = isr;
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
