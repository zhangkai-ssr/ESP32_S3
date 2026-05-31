/*
 * icm42670.c – ICM-42670-P 6-axis IMU driver for ESP-IDF 5.x
 *
 * Hardware (SHTB-V01 netlist):
 *   IMU_SDA → U4.15 = GPIO10
 *   IMU_SCL → U4.16 = GPIO11
 *   I2C address: 0x68 (AD0 = GND)
 *
 * Configuration used:
 *   Accel: ±4 g,   200 Hz ODR, Low-Noise mode
 *   Gyro:  ±500 dps, 200 Hz ODR, Low-Noise mode
 */

#include "icm42670.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "icm42670";

/* ---- Register map (Bank 0 / SREG) ---- */
#define REG_SIGNAL_PATH_RESET   0x02
#define REG_INT_STATUS          0x3A
#define REG_ACCEL_DATA_X1       0x0B
#define REG_PWR_MGMT0           0x1F
#define REG_GYRO_CONFIG0        0x20
#define REG_ACCEL_CONFIG0       0x21
#define REG_INTF_CONFIG1        0x36
#define REG_WHO_AM_I            0x75

/* Bit fields */
#define SOFT_RESET_EN           0x10  /* SIGNAL_PATH_RESET[4] */
#define INT_STATUS_RESET_DONE   0x10  /* INT_STATUS[4]        */
#define ACCEL_MODE_LN           0x03  /* PWR_MGMT0[1:0]  Low-Noise */
#define GYRO_MODE_LN            0x0C  /* PWR_MGMT0[3:2]  Low-Noise */
#define ACCEL_CFG_4G_200HZ      0x47  /* ±4 g, ODR 200 Hz  */
#define GYRO_CFG_500DPS_200HZ   0x47  /* ±500 dps, ODR 200 Hz */
#define WHO_AM_I_VAL            0x67

/* ---- I2C helpers ---- */
static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(ICM42670_I2C_PORT,
                                      ICM42670_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(10));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_write_read_device(ICM42670_I2C_PORT,
                                        ICM42670_I2C_ADDR,
                                        &reg, 1,
                                        val, 1,
                                        pdMS_TO_TICKS(10));
}

static esp_err_t burst_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(ICM42670_I2C_PORT,
                                        ICM42670_I2C_ADDR,
                                        &reg, 1,
                                        buf, len,
                                        pdMS_TO_TICKS(10));
}

/* ---- Public API ---- */
esp_err_t icm42670_init(void)
{
    esp_err_t ret;
    uint8_t   val;

    /* 1. Install I2C bus */
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = ICM42670_I2C_SDA,
        .scl_io_num       = ICM42670_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE, /* R40 external pull-up on board */
        .scl_pullup_en    = GPIO_PULLUP_DISABLE, /* R39 external pull-up on board */
        .master.clk_speed = ICM42670_I2C_FREQ_HZ,
    };
    ret = i2c_param_config(ICM42670_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %d", ret);
        return ret;
    }
    ret = i2c_driver_install(ICM42670_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "I2C%d ready (SDA=%d SCL=%d @ %d Hz)",
             ICM42670_I2C_PORT, ICM42670_I2C_SDA,
             ICM42670_I2C_SCL, ICM42670_I2C_FREQ_HZ);

    /* 2. WHO_AM_I */
    ret = read_reg(REG_WHO_AM_I, &val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %d  (addr=0x%02X, check wiring)",
                 ret, ICM42670_I2C_ADDR);
        return ret;
    }
    ESP_LOGI(TAG, "WHO_AM_I = 0x%02X (expect 0x%02X)", val, WHO_AM_I_VAL);
    if (val != WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch – check AD0 pin / I2C address");
        return ESP_ERR_NOT_FOUND;
    }

    /* 3. Soft reset */
    ret = write_reg(REG_SIGNAL_PATH_RESET, SOFT_RESET_EN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "soft reset failed: %d", ret);
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    /* Clear reset-done interrupt status */
    read_reg(REG_INT_STATUS, &val);
    if (!(val & INT_STATUS_RESET_DONE)) {
        ESP_LOGW(TAG, "reset-done flag not set (0x%02X)", val);
    }

    /* 4. Disable I3C interface (I2C-only mode) */
    ret = read_reg(REG_INTF_CONFIG1, &val);
    if (ret == ESP_OK) {
        val &= ~0x06; /* clear I3C_SDR_EN | I3C_DDR_EN */
        ret = write_reg(REG_INTF_CONFIG1, val);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INTF_CONFIG1 write failed: %d", ret);
        return ret;
    }

    /* 5. Accel config: ±4 g, 200 Hz ODR */
    ret = write_reg(REG_ACCEL_CONFIG0, ACCEL_CFG_4G_200HZ);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "accel config failed: %d", ret); return ret; }

    /* 6. Gyro config: ±500 dps, 200 Hz ODR */
    ret = write_reg(REG_GYRO_CONFIG0, GYRO_CFG_500DPS_200HZ);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "gyro config failed: %d", ret); return ret; }

    /* 7. Power on: accel Low-Noise + gyro Low-Noise */
    ret = write_reg(REG_PWR_MGMT0, ACCEL_MODE_LN | GYRO_MODE_LN);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "PWR_MGMT0 failed: %d", ret); return ret; }
    vTaskDelay(pdMS_TO_TICKS(50)); /* gyro startup ~40 ms */

    ESP_LOGI(TAG, "ICM-42670-P ready: ±%.0fg / ±%.0fdps / ODR%.0fHz",
             (double)ICM42670_ACCEL_FS_G,
             (double)ICM42670_GYRO_FS_DPS,
             (double)ICM42670_ODR_HZ);
    return ESP_OK;
}

esp_err_t icm42670_read_raw(icm42670_raw_t *out)
{
    uint8_t buf[12];
    esp_err_t ret = burst_read(REG_ACCEL_DATA_X1, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    out->ax = (int16_t)((buf[0]  << 8) | buf[1]);
    out->ay = (int16_t)((buf[2]  << 8) | buf[3]);
    out->az = (int16_t)((buf[4]  << 8) | buf[5]);
    out->gx = (int16_t)((buf[6]  << 8) | buf[7]);
    out->gy = (int16_t)((buf[8]  << 8) | buf[9]);
    out->gz = (int16_t)((buf[10] << 8) | buf[11]);
    return ESP_OK;
}
