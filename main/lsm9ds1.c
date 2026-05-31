#include "lsm9ds1.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_TIMEOUT_MS  50

static const char *TAG = "lsm9ds1";

/* ---- I2C helpers ---- */

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(LSM9DS1_I2C_PORT, cmd,
                                         pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* Burst read – I2C mode auto-increment is always on, plain register address. */
static esp_err_t i2c_read_regs(uint8_t addr, uint8_t reg,
                                uint8_t *buf, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);                           /* repeated START */
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(LSM9DS1_I2C_PORT, cmd,
                                         pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

/* ---- Public API ---- */

esp_err_t lsm9ds1_read_who_am_i(uint8_t *ag_id, uint8_t *m_id)
{
    esp_err_t ret;
    if (ag_id) {
        ret = i2c_read_regs(LSM9DS1_ADDR_AG, LSM9DS1_REG_WHO_AM_I, ag_id, 1);
        if (ret != ESP_OK) return ret;
    }
    if (m_id) {
        ret = i2c_read_regs(LSM9DS1_ADDR_M, LSM9DS1_REG_WHO_AM_I_M, m_id, 1);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

esp_err_t lsm9ds1_init(void)
{
    esp_err_t ret;

    /* External 4.7 kΩ pull-ups (R39/R40) → disable internal pull-ups */
    const i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = LSM9DS1_PIN_SDA,
        .scl_io_num       = LSM9DS1_PIN_SCL,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE,
        .scl_pullup_en    = GPIO_PULLUP_DISABLE,
        .master.clk_speed = LSM9DS1_I2C_FREQ_HZ,
    };
    ret = i2c_param_config(LSM9DS1_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(LSM9DS1_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Verify WHO_AM_I with retries */
    uint8_t ag_id = 0, m_id = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        ret = lsm9ds1_read_who_am_i(&ag_id, &m_id);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "WHO_AM_I: AG=0x%02X (expect 0x%02X)  M=0x%02X (expect 0x%02X)",
                     ag_id, LSM9DS1_WHO_AM_I_AG_EXP, m_id, LSM9DS1_WHO_AM_I_M_EXP);
            if (ag_id == LSM9DS1_WHO_AM_I_AG_EXP && m_id == LSM9DS1_WHO_AM_I_M_EXP) {
                break;
            }
            ESP_LOGW(TAG, "unexpected WHO_AM_I, retry %d", attempt + 1);
        } else {
            ESP_LOGW(TAG, "WHO_AM_I read failed (attempt %d): %s",
                     attempt, esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (ag_id != LSM9DS1_WHO_AM_I_AG_EXP || m_id != LSM9DS1_WHO_AM_I_M_EXP) {
        /* Scan I2C bus to show what devices are actually present */
        ESP_LOGE(TAG, "LSM9DS1 not found (AG=0x%02X M=0x%02X) – scanning bus...", ag_id, m_id);
        int found = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(cmd);
            esp_err_t r = i2c_master_cmd_begin(LSM9DS1_I2C_PORT, cmd, pdMS_TO_TICKS(10));
            i2c_cmd_link_delete(cmd);
            if (r == ESP_OK) {
                ESP_LOGI(TAG, "  I2C device found at 0x%02X", addr);
                found++;
            }
        }
        if (found == 0) {
            ESP_LOGE(TAG, "  No I2C devices found – chip not populated or wiring issue");
        }
        return ESP_ERR_NOT_FOUND;
    }

    /* Software reset */
    ret = i2c_write_reg(LSM9DS1_ADDR_AG, LSM9DS1_REG_CTRL_REG8, 0x01);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Magnetometer reboot + soft reset */
    ret = i2c_write_reg(LSM9DS1_ADDR_M, LSM9DS1_REG_CTRL_REG2_M, 0x0C);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    /*
     * Gyroscope configuration:
     *   CTRL_REG1_G = 0x88
     *     ODR_G [7:5] = 100  → 238 Hz
     *     FS_G  [4:3] = 01   → ±500 dps
     *     BW_G  [1:0] = 00
     */
    ret = i2c_write_reg(LSM9DS1_ADDR_AG, LSM9DS1_REG_CTRL_REG1_G, 0x88);
    if (ret != ESP_OK) return ret;

    /* CTRL_REG3_G: HP filter off */
    ret = i2c_write_reg(LSM9DS1_ADDR_AG, LSM9DS1_REG_CTRL_REG3_G, 0x00);
    if (ret != ESP_OK) return ret;

    /* CTRL_REG4: enable all gyro axes (Zen/Yen/Xen = 1) → 0x38 */
    ret = i2c_write_reg(LSM9DS1_ADDR_AG, LSM9DS1_REG_CTRL_REG4, 0x38);
    if (ret != ESP_OK) return ret;

    /* CTRL_REG5_XL: enable all accel axes → 0x38 */
    ret = i2c_write_reg(LSM9DS1_ADDR_AG, LSM9DS1_REG_CTRL_REG5_XL, 0x38);
    if (ret != ESP_OK) return ret;

    /*
     * Accelerometer configuration:
     *   CTRL_REG6_XL = 0x93
     *     ODR_XL       [7:5] = 100  → 238 Hz
     *     FS_XL        [4:3] = 10   → ±4 g
     *     BW_SCAL_ODR  [2]   = 0
     *     BW_XL        [1:0] = 11   → 50 Hz AA filter
     */
    ret = i2c_write_reg(LSM9DS1_ADDR_AG, LSM9DS1_REG_CTRL_REG6_XL, 0x93);
    if (ret != ESP_OK) return ret;

    /*
     * Magnetometer configuration:
     *   CTRL_REG1_M = 0xF0
     *     TEMP_COMP [7]   = 1    → temperature compensation on
     *     OM        [6:5] = 11   → ultra-high performance XY
     *     DO        [4:2] = 100  → 40 Hz ODR
     *   CTRL_REG2_M = 0x20      → ±8 gauss (FS[6:5] = 01)
     *   CTRL_REG3_M = 0x00      → continuous-conversion mode
     *   CTRL_REG4_M = 0x0C      → ultra-high performance Z (OMZ[3:2] = 11)
     *   CTRL_REG5_M = 0x40      → block data update enabled
     */
    ret = i2c_write_reg(LSM9DS1_ADDR_M, LSM9DS1_REG_CTRL_REG1_M, 0xF0);
    if (ret != ESP_OK) return ret;

    ret = i2c_write_reg(LSM9DS1_ADDR_M, LSM9DS1_REG_CTRL_REG2_M, 0x20);
    if (ret != ESP_OK) return ret;

    ret = i2c_write_reg(LSM9DS1_ADDR_M, LSM9DS1_REG_CTRL_REG3_M, 0x00);
    if (ret != ESP_OK) return ret;

    ret = i2c_write_reg(LSM9DS1_ADDR_M, LSM9DS1_REG_CTRL_REG4_M, 0x0C);
    if (ret != ESP_OK) return ret;

    ret = i2c_write_reg(LSM9DS1_ADDR_M, LSM9DS1_REG_CTRL_REG5_M, 0x40);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "init done (AG: 238 Hz ±4g/±500dps  M: 40 Hz ±8 gauss)");
    return ESP_OK;
}

esp_err_t lsm9ds1_read(lsm9ds1_data_t *data)
{
    uint8_t buf[6];
    esp_err_t ret;

    /* Read gyroscope: OUT_X_L_G … OUT_Z_H_G (6 bytes) */
    ret = i2c_read_regs(LSM9DS1_ADDR_AG, LSM9DS1_REG_OUT_X_L_G, buf, 6);
    if (ret != ESP_OK) return ret;
    data->gx = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    data->gy = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    data->gz = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);

    /* Read accelerometer: OUT_X_L_XL … OUT_Z_H_XL (6 bytes) */
    ret = i2c_read_regs(LSM9DS1_ADDR_AG, LSM9DS1_REG_OUT_X_L_XL, buf, 6);
    if (ret != ESP_OK) return ret;
    data->ax = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    data->ay = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    data->az = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);

    /* Read magnetometer: OUT_X_L_M … OUT_Z_H_M (6 bytes) */
    ret = i2c_read_regs(LSM9DS1_ADDR_M, LSM9DS1_REG_OUT_X_L_M, buf, 6);
    if (ret != ESP_OK) return ret;
    data->mx = (int16_t)((uint16_t)buf[1] << 8 | buf[0]);
    data->my = (int16_t)((uint16_t)buf[3] << 8 | buf[2]);
    data->mz = (int16_t)((uint16_t)buf[5] << 8 | buf[4]);

    return ESP_OK;
}
