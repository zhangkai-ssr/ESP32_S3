#ifndef PMIC_NPM1300_H_
#define PMIC_NPM1300_H_

/*
 * pmic_npm1300.h – nPM1300 PMIC driver for ESP32-S3 / ESP-IDF 5.x
 *
 * Hardware (SHTB-V01 netlist):
 *   PMIC_I2C_SDA → U4.24 = GPIO18
 *   PMIC_I2C_SCL → U4.23 = GPIO17
 *   PMIC_INT     → U4.27 = GPIO19
 *   I2C address  : 0x6B (nPM1300 default)
 *
 * Ported from nRF54L15 Zephyr driver (pmic_npm1300.c/.h).
 * Platform dependencies replaced:
 *   mfd_npm1300_reg_*  → direct ESP-IDF I2C transactions
 *   k_work_delayable   → FreeRTOS software timer
 *   k_msleep           → vTaskDelay
 *   LOG_*              → ESP_LOG*
 *   pm_device_action_run → no-op stub (ESP-IDF has no Zephyr PM device model)
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- GPIO / I2C pin assignments ---- */
#define PMIC_I2C_PORT    1          /* I2C_NUM_1  (I2C_NUM_0 used by ICM-42670) */
#define PMIC_I2C_SDA     18
#define PMIC_I2C_SCL     17
#define PMIC_I2C_FREQ_HZ 400000
#define PMIC_I2C_ADDR    0x6B      /* nPM1300 7-bit address */
#define PMIC_INT_PIN     19        /* PMIC_INT → GPIO19 */

/* ---- Public API ---- */

/**
 * @brief Initialise the nPM1300 PMIC.
 *
 * Installs the I2C bus, configures BUCK2 / LDO1 outputs,
 * sets up GPIO0 as interrupt output, disables NTC monitoring,
 * enables the charger, and starts the periodic charger-poll timer.
 *
 * @return ESP_OK on success, or a negative esp_err_t on failure.
 */
esp_err_t pmic_npm1300_init(void);

/**
 * @brief Re-apply all register configuration (idempotent).
 *
 * Call after any reset or wake-from-ship-mode to restore full
 * PMIC state (BUCK2 3.3 V, LDO1 3.3 V, charger enabled, etc.).
 *
 * @return ESP_OK on success.
 */
esp_err_t pmic_npm1300_reconfigure(void);

/* Get last-known battery percentage (0–100). */
uint8_t pmic_npm1300_get_battery_pct(void);

/* Charging state (updated every ~2 s by the charger-poll timer). */
typedef enum {
    PMIC_CHARGE_NONE = 0,    /* No charger connected (VBUS absent) */
    PMIC_CHARGE_ACTIVE,      /* Charging in progress (trickle / CC / CV / recharge) */
    PMIC_CHARGE_COMPLETE,    /* VBUS present + battery full */
} pmic_charge_state_t;

pmic_charge_state_t pmic_npm1300_get_charge_state(void);

/**
 * @brief Read and clear SHPHLD button edge events.
 *
 * Sets *pressed / *released to true if the respective event occurred
 * since the last poll.  Safe to call from any task.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if PMIC not ready.
 */
esp_err_t pmic_npm1300_poll_shphld(bool *pressed, bool *released);

/** @return true if the SHPHLD button is currently held down. */
bool pmic_npm1300_is_shphld_pressed(void);

/**
 * @brief Enter nPM1300 ship mode (deep sleep / power-off).
 *
 * Calls mfd_npm1300_hibernate equivalent.  Under normal conditions
 * this function does NOT return (MCU loses power).
 * Only a SHPHLD long-press wakes the device.
 *
 * @return Negative esp_err_t on failure; does not return on success.
 */
esp_err_t pmic_npm1300_enter_ship_mode(void);

/**
 * @brief Check (and consume) the SHPHLD PRESS wakeup event.
 *
 * After ship-mode wake, the PMIC SHPHLD event register holds a PRESS
 * bit.  This function reads and clears it; use in boot-guard logic.
 *
 * @return true if a PRESS wakeup event was present.
 */
bool pmic_npm1300_is_shphld_wakeup(void);

/**
 * @brief Optional callback invoked when battery percentage is refreshed.
 *
 * Register with pmic_npm1300_set_battery_cb().  Called from the
 * charger-poll FreeRTOS timer callback context (not an ISR).
 */
typedef void (*pmic_battery_cb_t)(uint8_t pct);

void pmic_npm1300_set_battery_cb(pmic_battery_cb_t cb);

#endif /* PMIC_NPM1300_H_ */
