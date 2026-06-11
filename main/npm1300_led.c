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

/* ---- nPM1300 register pages ---- */
#define NPM1300_LED_PAGE        0x66    /* LED page (verified empirically) */
#define NPM1300_BUCK_PAGE       0x04    /* BUCK regulators */
#define NPM1300_LDSW_PAGE       0x08    /* LDSW / LDO regulators */

/* ---- BUCK page (0x04) offsets ---- */
#define NPM1300_BUCK1_ENASET    0x00    /* write 1 to enable BUCK1 */
#define NPM1300_BUCK1_ENACLR    0x01
#define NPM1300_BUCK2_ENASET    0x02
#define NPM1300_BUCK2_ENACLR    0x03
#define NPM1300_BUCK1_VOUTNORM  0x08    /* voltage = 1000 + val*100 mV */
#define NPM1300_BUCK2_VOUTNORM  0x0A
#define NPM1300_BUCK_CTRL0      0x15
#define NPM1300_BUCK_STATUS     0x34

/* ---- LDSW / LDO page (0x08) offsets ---- */
#define NPM1300_LDSW1_ENASET    0x00    /* TASKLDSW1SET — enable LDO1 */
#define NPM1300_LDSW1_ENACLR    0x01
#define NPM1300_LDSW2_ENASET    0x02    /* TASKLDSW2SET — enable LDO2 */
#define NPM1300_LDSW2_ENACLR    0x03
#define NPM1300_LDSW_STATUS     0x04    /* bit0=LDO1 on, bit2=LDO2 on */
#define NPM1300_LDSW_LDOSEL     0x08    /* bit0=LDO1 LDO/LDSW, bit1=LDO2 */
#define NPM1300_LDSW1_VOUTSEL   0x0C    /* voltage = 1000 + val*100 mV */
#define NPM1300_LDSW2_VOUTSEL   0x0D

/* Voltage encoding: voutsel = (mV - 1000) / 100, range 1000..3300 mV */
#define NPM1300_VOUTSEL_3V3     0x17    /* 3300 mV */

/* ---- LED page offsets ---- */
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

/* Write a register and read it back, retry up to 3 times if mismatched.
 * Some PMIC registers have reserved bits that may read back differently,
 * so caller passes a mask of bits to verify. Returns ESP_OK on success. */
static esp_err_t npm1300_write_verify(const char *tag, uint8_t page,
                                      uint8_t offset, uint8_t expected,
                                      uint8_t mask)
{
    uint8_t got;
    esp_err_t ret;
    for (int i = 0; i < 3; i++) {
        ret = npm1300_write(page, offset, expected);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s write try %d failed: %s", tag, i, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        uint8_t cmd[2] = { page, offset };
        ret = i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                           cmd, sizeof(cmd), &got, 1,
                                           pdMS_TO_TICKS(10));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s readback try %d failed: %s", tag, i, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if ((got & mask) == (expected & mask)) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s mismatch try %d: wrote=0x%02X got=0x%02X (mask=0x%02X)",
                 tag, i, expected, got, mask);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "%s give up after 3 tries", tag);
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t npm1300_enable_sensor_rails(void)
{
    esp_err_t ret;
    uint8_t val;

    ESP_LOGI(TAG, "enabling NPM1300 sensor rails (BUCK1+LDO1+LDO2 @ 3.3V)");

    /* ---- BUCK1: 3.3V for IMU + FLASH ---- */
    ret = npm1300_write_verify("BUCK1_VOUT",  NPM1300_BUCK_PAGE,
                               NPM1300_BUCK1_VOUTNORM, NPM1300_VOUTSEL_3V3, 0xFF);
    if (ret != ESP_OK) return ret;
    ret = npm1300_write(NPM1300_BUCK_PAGE, NPM1300_BUCK1_ENASET, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BUCK1 enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "BUCK1 enabled @ 3300mV (IMU+FLASH rail)");

    /* ---- LDO1 + LDO2 ---- */
    /* Mode: LDO1 = LDO, LDO2 = LDO (bit0 + bit1 set) — needed for adjustable
     * voltage; LDSW mode just passes BUCK output through unregulated. */
    ret = npm1300_write_verify("LDOSEL", NPM1300_LDSW_PAGE,
                               NPM1300_LDSW_LDOSEL, 0x03, 0x03);
    if (ret != ESP_OK) return ret;

    ret = npm1300_write_verify("LDO1_VOUT", NPM1300_LDSW_PAGE,
                               NPM1300_LDSW1_VOUTSEL, NPM1300_VOUTSEL_3V3, 0xFF);
    if (ret != ESP_OK) return ret;
    ret = npm1300_write_verify("LDO2_VOUT", NPM1300_LDSW_PAGE,
                               NPM1300_LDSW2_VOUTSEL, NPM1300_VOUTSEL_3V3, 0xFF);
    if (ret != ESP_OK) return ret;

    ret = npm1300_write(NPM1300_LDSW_PAGE, NPM1300_LDSW1_ENASET, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO1 enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = npm1300_write(NPM1300_LDSW_PAGE, NPM1300_LDSW2_ENASET, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO2 enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "LDO1 + LDO2 enabled @ 3300mV (ADS1298-A + ADS1298-B rails)");

    /* Wait for rails to settle before downstream chips are accessed.
     * NPM1300 BUCK/LDO startup typically completes in <2 ms, but ADS1298
     * needs additional time after its AVDD comes up before SPI is reliable. */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ---- Dump status for verification ---- */
    uint8_t cmd[2];
    cmd[0] = NPM1300_BUCK_PAGE; cmd[1] = NPM1300_BUCK_STATUS;
    if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                     cmd, 2, &val, 1, pdMS_TO_TICKS(10)) == ESP_OK) {
        ESP_LOGI(TAG, "BUCK_STATUS = 0x%02X (BUCK1 %s, BUCK2 %s)", val,
                 (val & 0x04) ? "ON" : "OFF",
                 (val & 0x40) ? "ON" : "OFF");
    }
    cmd[0] = NPM1300_LDSW_PAGE; cmd[1] = NPM1300_LDSW_STATUS;
    if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                     cmd, 2, &val, 1, pdMS_TO_TICKS(10)) == ESP_OK) {
        ESP_LOGI(TAG, "LDSW_STATUS = 0x%02X (LDO1 %s, LDO2 %s)", val,
                 (val & 0x03) ? "ON" : "OFF",
                 (val & 0x0C) ? "ON" : "OFF");
    }
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
