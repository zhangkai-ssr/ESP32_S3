#ifndef LSM9DS1_H_
#define LSM9DS1_H_

#include <stdint.h>
#include "esp_err.h"

/* ---- GPIO pin assignments (from hardware netlist SHTB-V01) ----
 * IMU_SDA : GPIO11,  4.7kΩ pull-up via R40
 * IMU_SCL : GPIO10,  4.7kΩ pull-up via R39 */
#define LSM9DS1_PIN_SDA       11
#define LSM9DS1_PIN_SCL       10

/* ---- I2C bus configuration ---- */
#define LSM9DS1_I2C_PORT      I2C_NUM_0
#define LSM9DS1_I2C_FREQ_HZ   400000

/* ---- I2C slave addresses ----
 * SDO_AG tied to W3.3V (U5.6) → SA0 = 1 → AG  addr 0x6B
 * SDO_M  tied to W3.3V (U5.1) → SA1 = 1 → Mag addr 0x1E */
#define LSM9DS1_ADDR_AG       0x6B
#define LSM9DS1_ADDR_M        0x1E

/* ---- Accel/Gyro register map ---- */
#define LSM9DS1_REG_WHO_AM_I        0x0F   /* expected: 0x68 */
#define LSM9DS1_REG_CTRL_REG1_G     0x10   /* gyro ODR / FS / BW */
#define LSM9DS1_REG_CTRL_REG2_G     0x11
#define LSM9DS1_REG_CTRL_REG3_G     0x12
#define LSM9DS1_REG_CTRL_REG4       0x1E   /* gyro / XL axis enable */
#define LSM9DS1_REG_CTRL_REG5_XL    0x1F   /* XL axis enable */
#define LSM9DS1_REG_CTRL_REG6_XL    0x20   /* XL ODR / FS / BW */
#define LSM9DS1_REG_CTRL_REG7_XL    0x21
#define LSM9DS1_REG_CTRL_REG8       0x22   /* SW_RESET bit */
#define LSM9DS1_REG_CTRL_REG9       0x23
#define LSM9DS1_REG_STATUS_REG      0x27
#define LSM9DS1_REG_OUT_X_L_G       0x18   /* gyro X low byte (burst: X,Y,Z) */
#define LSM9DS1_REG_OUT_X_L_XL      0x28   /* accel X low byte (burst: X,Y,Z) */

/* ---- Magnetometer register map ---- */
#define LSM9DS1_REG_WHO_AM_I_M      0x0F   /* expected: 0x3D */
#define LSM9DS1_REG_CTRL_REG1_M     0x20   /* mag temp-comp / perf / ODR */
#define LSM9DS1_REG_CTRL_REG2_M     0x21   /* mag FS / REBOOT / SOFT_RST */
#define LSM9DS1_REG_CTRL_REG3_M     0x22   /* operating mode */
#define LSM9DS1_REG_CTRL_REG4_M     0x23   /* Z performance */
#define LSM9DS1_REG_CTRL_REG5_M     0x24   /* BDU */
#define LSM9DS1_REG_STATUS_REG_M    0x27
#define LSM9DS1_REG_OUT_X_L_M       0x28   /* mag X low byte (burst: X,Y,Z) */

/* ---- Expected WHO_AM_I responses ---- */
#define LSM9DS1_WHO_AM_I_AG_EXP     0x68
#define LSM9DS1_WHO_AM_I_M_EXP      0x3D

/* ---- Sensor data (raw 16-bit signed, little-endian, matching doc §5.2) ---- */
typedef struct {
    int16_t ax, ay, az;   /* accelerometer  (configured: ±4 g,    238 Hz) */
    int16_t gx, gy, gz;   /* gyroscope      (configured: ±500 dps, 238 Hz) */
    int16_t mx, my, mz;   /* magnetometer   (configured: ±8 gauss,  40 Hz) */
} lsm9ds1_data_t;

esp_err_t lsm9ds1_init(void);
esp_err_t lsm9ds1_read(lsm9ds1_data_t *data);
esp_err_t lsm9ds1_read_who_am_i(uint8_t *ag_id, uint8_t *m_id);

#endif /* LSM9DS1_H_ */
