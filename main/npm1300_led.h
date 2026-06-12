/*
 * npm1300_led.h – nPM1300 PMIC LED driver (hardware abstraction layer)
 *
 * Hardware (SHTB-V01 netlist):
 *   PMIC_I2C_SCL → U4.23 = GPIO17,  10 kΩ pull-up via R33
 *   PMIC_I2C_SDA → U4.24 = GPIO18,  10 kΩ pull-up via R34
 *   nPM1300 I2C address : 0x6B (fixed)
 *
 * LED channel ↔ colour mapping (from netlist):
 *   LED0 (U14.25) = BLUE
 *   LED1 (U14.26) = GREEN
 *   LED2 (U14.27) = RED
 *
 * All three channels are configured in HOST (software-controlled) mode.
 * Use npm1300_led_set_rgb() to drive each channel on or off.
 * Brightness modulation (software PWM) is handled by the led_ctrl layer.
 */

#ifndef NPM1300_LED_H_
#define NPM1300_LED_H_

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c.h"

/* ---- I2C bus configuration ---- */
#define NPM1300_I2C_PORT        I2C_NUM_1
#define NPM1300_I2C_SCL         17          /* U4.23 */
#define NPM1300_I2C_SDA         18          /* U4.24 */
#define NPM1300_I2C_FREQ_HZ     400000
#define NPM1300_I2C_ADDR        0x6B        /* nPM1300 fixed 7-bit address */

/**
 * @brief  Install the I2C driver on I2C_NUM_1 and configure all three
 *         LED channels in HOST (software-controlled) mode.
 *         Leaves all LEDs off.
 * @return ESP_OK on success, or an esp_err_t if the PMIC cannot be reached.
 */
esp_err_t npm1300_led_init(void);

/**
 * @brief  Enable BUCK1 (3.3V for IMU + FLASH), LDO1 (3.3V for ADS1298-A),
 *         LDO2 (3.3V for ADS1298-B). Must be called AFTER npm1300_led_init()
 *         (which installs the I2C driver) and BEFORE ads1298_init() /
 *         lsm9ds1_init().
 *
 *         Per the schematic on this board:
 *           BUCK1 → IMU + FLASH (3.3V rail)
 *           LDO1  → ADS1298-A + YOSC-A
 *           LDO2  → ADS1298-B + YOSC-B
 *
 *         NPM1300 factory default leaves these rails DISABLED. Firmware must
 *         configure mode (LDSW vs LDO) + voltage + enable explicitly.
 *
 * @return ESP_OK on success.
 */
esp_err_t npm1300_enable_sensor_rails(void);

/**
 * @brief  Enable the NPM1300 battery charger.
 *
 * NPM1300 power-on default leaves the charger DISABLED, and charging current
 * (BCHGISET) reset value is 0 — so charger must be explicitly configured even
 * before being enabled. Without this call, plugging USB will NOT charge the
 * battery, and with a depleted cell the system will brown-out as soon as WiFi
 * PA starts (VBUS ILIM alone cannot supply the peak current).
 *
 * Configuration (matches the nrf54l15_blinky4 reference project):
 *   - Charging current      : 150 mA   (BCHGISETMSB/LSB)
 *   - Termination voltage   : 4.15 V   (BCHGVTERM)
 *   - Warm-temp termination : 4.00 V   (BCHGVTERMR)
 *   - NTC monitoring        : disabled (NTC pin tied to ground on this board)
 *
 * Call this very early in app_main() — BEFORE npm1300_enable_sensor_rails()
 * and BEFORE wifi/PHY init — so the battery starts charging immediately and
 * has the best chance of supplementing VSYS by the time WiFi PA fires up.
 *
 * @return ESP_OK on success.
 */
esp_err_t npm1300_enable_charger(void);

/**
 * @brief  Read and log charger status (VBUS presence + BCHG state machine).
 *         Safe to call from the main heartbeat loop; takes a few I2C reads
 *         on the PMIC bus.
 */
void npm1300_log_charger_status(void);

/**
 * @brief  Directly set the on/off state of each RGB channel.
 *         This is the only hardware write path; call it only from the
 *         led_ctrl layer to keep I2C traffic minimal.
 * @param  r  true = RED   channel on
 * @param  g  true = GREEN channel on
 * @param  b  true = BLUE  channel on
 */
void npm1300_led_set_rgb(bool r, bool g, bool b);

#endif /* NPM1300_LED_H_ */
