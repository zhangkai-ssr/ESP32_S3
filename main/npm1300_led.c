/*
 * npm1300_led.c – nPM1300 PMIC LED driver (hardware abstraction layer)
 *
 * Register map (nPM1300 PS, LED page 0x66):
 *
 *   offset | register      | description
 *   -------|---------------|--------------------------------------------
 *   0x00   | LEDDRV0CONFIG | LED0 (BLUE)  mode  [1:0]  3=HOST
 *   0x01   | LEDDRV1CONFIG | LED1 (GREEN) mode  [1:0]  3=HOST
 *   0x02   | LEDDRV2CONFIG | LED2 (RED)   mode  [1:0]  3=HOST
 *   0x03   | LEDDRV0SET    | Write 1 → LED0 ON  (HOST mode only)
 *   0x04   | LEDDRV0CLR    | Write 1 → LED0 OFF
 *   0x05   | LEDDRV1SET    | Write 1 → LED1 ON
 *   0x06   | LEDDRV1CLR    | Write 1 → LED1 OFF
 *   0x07   | LEDDRV2SET    | Write 1 → LED2 ON
 *   0x08   | LEDDRV2CLR    | Write 1 → LED2 OFF
 *
 * I2C transaction format:  [DEV_ADDR | W] [PAGE] [OFFSET] [VALUE]
 */

#include "npm1300_led.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "npm1300_led";

/* ---- nPM1300 register definitions ---- */
#define NPM1300_LED_PAGE        0x66

#define NPM1300_LED0CONFIG      0x00    /* LEDDRV0CONFIG */
#define NPM1300_LED1CONFIG      0x01    /* LEDDRV1CONFIG */
#define NPM1300_LED2CONFIG      0x02    /* LEDDRV2CONFIG */
#define NPM1300_LED0SET         0x03    /* LEDDRV0SET    */
#define NPM1300_LED0CLR         0x04    /* LEDDRV0CLR    */
#define NPM1300_LED1SET         0x05    /* LEDDRV1SET    */
#define NPM1300_LED1CLR         0x06    /* LEDDRV1CLR    */
#define NPM1300_LED2SET         0x07    /* LEDDRV2SET    */
#define NPM1300_LED2CLR         0x08    /* LEDDRV2CLR    */

#define NPM1300_LED_MODE_HOST   0x03    /* software-controlled */

/* ---- I2C helper ---- */

static esp_err_t npm1300_write(uint8_t page, uint8_t offset, uint8_t val)
{
    uint8_t buf[3] = { page, offset, val };
    return i2c_master_write_to_device(NPM1300_I2C_PORT,
                                      NPM1300_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(10));
}

/* ---- Public API ---- */

esp_err_t npm1300_led_init(void)
{
    esp_err_t ret;

    /* Install I2C master driver on I2C_NUM_1 (PMIC bus, separate from IMU) */
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = NPM1300_I2C_SDA,
        .scl_io_num       = NPM1300_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE,    /* R34: external 10 kΩ */
        .scl_pullup_en    = GPIO_PULLUP_DISABLE,    /* R33: external 10 kΩ */
        .master.clk_speed = NPM1300_I2C_FREQ_HZ,
    };

    ret = i2c_param_config(NPM1300_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(NPM1300_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C%d ready (SDA=%d SCL=%d @ %d Hz, PMIC addr=0x%02X)",
             NPM1300_I2C_PORT, NPM1300_I2C_SDA, NPM1300_I2C_SCL,
             NPM1300_I2C_FREQ_HZ, NPM1300_I2C_ADDR);

    /* Configure all three LED channels in HOST (software-controlled) mode */
    ret = npm1300_write(NPM1300_LED_PAGE, NPM1300_LED0CONFIG, NPM1300_LED_MODE_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED0 config failed: %s (check PMIC I2C wiring)",
                 esp_err_to_name(ret));
        return ret;
    }
    ret = npm1300_write(NPM1300_LED_PAGE, NPM1300_LED1CONFIG, NPM1300_LED_MODE_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED1 config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = npm1300_write(NPM1300_LED_PAGE, NPM1300_LED2CONFIG, NPM1300_LED_MODE_HOST);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED2 config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Ensure all LEDs are off after init */
    npm1300_led_set_rgb(false, false, false);

    ESP_LOGI(TAG, "nPM1300 LED driver ready (HOST mode, all channels off)");
    return ESP_OK;
}

void npm1300_led_set_rgb(bool r, bool g, bool b)
{
    /* LED0 = BLUE  (U14.25) */
    npm1300_write(NPM1300_LED_PAGE,
                  b ? NPM1300_LED0SET : NPM1300_LED0CLR,
                  0x01);
    /* LED1 = GREEN (U14.26) */
    npm1300_write(NPM1300_LED_PAGE,
                  g ? NPM1300_LED1SET : NPM1300_LED1CLR,
                  0x01);
    /* LED2 = RED   (U14.27) */
    npm1300_write(NPM1300_LED_PAGE,
                  r ? NPM1300_LED2SET : NPM1300_LED2CLR,
                  0x01);
}
