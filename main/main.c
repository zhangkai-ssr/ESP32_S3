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
#include "flash.h"
#include "ota_update.h"

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
    /* Give the host serial monitor 1.5s to sync — otherwise on a fast boot
     * we lose the first ~1s of log output to the bytestream-resync delay. */
    vTaskDelay(pdMS_TO_TICKS(1500));

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

    /* Enable the battery charger FIRST. NPM1300 power-on default has the
     * charger disabled AND BCHGISET = 0, so plugging USB does nothing for
     * battery charge. With a depleted cell, WiFi PA startup current then
     * causes a brown-out loop. Running this before WiFi/PHY init lets the
     * charger start sourcing into the battery as soon as possible so VSYS
     * has battery-supplemented headroom by the time the radio fires up. */
    if (npm1300_enable_charger() != ESP_OK) {
        ESP_LOGE(TAG, "charger enable FAILED — battery will not charge from USB");
    }
    log_memory_snapshot("charger_enabled");

    /* Enable BUCK1/LDO1/LDO2 BEFORE talking to sensors. NPM1300 default
     * leaves these rails OFF; without this call ADS1298 has no AVDD and
     * IMU has no power. Must happen AFTER led_ctrl_init() (which installs
     * the I2C bus driver). Failure here is non-fatal — we still let the
     * board boot WiFi so we can SSH-debug. */
    if (npm1300_enable_sensor_rails() != ESP_OK) {
        ESP_LOGE(TAG, "sensor rails enable FAILED — sensors will not respond");
    }
    log_memory_snapshot("rails_enabled");

    /* Probe the W25Q16 SPI flash (SPI3_HOST). Diagnostic only — non-fatal:
     * if the chip is absent or miswired we still boot the link. */
    ESP_LOGI(TAG, "Probing W25Q16 flash...");
    ret = flash_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "flash probe failed (%s) — running bit-bang diagnostic",
                 esp_err_to_name(ret));
        flash_gpio_clock_test();
    }
    log_memory_snapshot("flash_probed");

    wifi_manager_init();
    wifi_manager_wait_connected();
    log_memory_snapshot("wifi_connected");

    /* Bring the OTA push server up right after Wi-Fi — before sensor init —
     * so even a firmware that later crashes in ADS1298/IMU bring-up still
     * exposes a recovery channel on TCP:3335 (ota_push.py). */
    ota_update_init();

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
    /* System fully up. If this boot is the first run of a freshly OTA'd image
     * (PENDING_VERIFY), confirm it now so the bootloader won't roll back.
     * Placed last on purpose: a new image that crash-loops in bring-up never
     * reaches here, so the bootloader auto-reverts to the previous slot. */
    ota_update_mark_valid();

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
        npm1300_log_charger_status();
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}
