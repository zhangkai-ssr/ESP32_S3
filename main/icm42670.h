#ifndef ICM42670_H_
#define ICM42670_H_

#include <stdint.h>
#include "esp_err.h"

/* ---- GPIO pin assignments (from hardware netlist SHTB-V01) ---- */
/* IMU_SDA → U4.15 = GPIO10,  IMU_SCL → U4.16 = GPIO11             */
#define ICM42670_I2C_PORT    0          /* I2C_NUM_0 */
#define ICM42670_I2C_SDA     10
#define ICM42670_I2C_SCL     11
#define ICM42670_I2C_FREQ_HZ 400000
#define ICM42670_I2C_ADDR    0x68      /* AD0 = GND */

/* Full-scale ranges used in this configuration */
#define ICM42670_ACCEL_FS_G     4.0f   /* ±4 g  */
#define ICM42670_GYRO_FS_DPS  500.0f   /* ±500 dps */
#define ICM42670_ODR_HZ       200.0f

/* Raw 6-axis sample (straight from the sensor registers) */
typedef struct {
    int16_t ax, ay, az;  /* accelerometer counts */
    int16_t gx, gy, gz;  /* gyroscope counts     */
} icm42670_raw_t;

/**
 * @brief  Install I2C driver and bring ICM-42670-P out of reset.
 *         Must be called once before icm42670_read_raw().
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if WHO_AM_I mismatch,
 *         or other negative esp_err_t on I2C failure.
 */
esp_err_t icm42670_init(void);

/**
 * @brief  Burst-read one 6-axis sample from the sensor.
 * @param  out  Destination struct; unchanged on error.
 * @return ESP_OK on success.
 */
esp_err_t icm42670_read_raw(icm42670_raw_t *out);

#endif /* ICM42670_H_ */
