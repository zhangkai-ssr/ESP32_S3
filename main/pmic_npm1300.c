/*
 * pmic_npm1300.c – nPM1300 PMIC driver for ESP32-S3 / ESP-IDF 5.x
 *
 * Ported from the nRF54L15 Zephyr implementation.
 *
 * Platform mapping:
 *   mfd_npm1300_reg_read/write/read_burst  → npm1300_reg_read / _write / _burst_read
 *   k_work_delayable / k_work_reschedule   → FreeRTOS xTimerCreate / xTimerChangePeriod
 *   k_msleep                               → vTaskDelay(pdMS_TO_TICKS(...))
 *   LOG_INF / LOG_WRN / LOG_ERR            → ESP_LOGI / ESP_LOGW / ESP_LOGE
 *   pm_device_action_run (I2C suspend)     → no-op stub (not needed on ESP-IDF)
 *   ble_app_update_battery_level           → optional pmic_battery_cb_t callback
 *   Zephyr GPIO interrupt API              → ESP-IDF gpio_isr_handler_add
 */

#include "pmic_npm1300.h"

#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "pmic";

/* ---- Diagnostics toggle (set 1 to force 1 Hz charger log) ---- */
#define PMIC_DIAG_LOG 0

/* ------------------------------------------------------------------ */
/* nPM1300 register bases                                              */
/* ------------------------------------------------------------------ */
#define MAIN_EVT_BASE  0x00U
#define VBUS_BASE      0x02U
#define CHGR_BASE      0x03U
#define ADC_BASE       0x05U
#define BUCK_BASE      0x04U
#define GPIO_BASE      0x06U
#define LDSW_BASE      0x08U
#define LED_BASE       0x0AU

/* MAIN (SHPHLD) */
#define SHPHLD_EVT_OFFSET_SET    0x12U
#define SHPHLD_EVT_OFFSET_CLR    0x13U
#define SHPHLD_EVT_PRESS_MASK    0x01U
#define SHPHLD_EVT_RELEASE_MASK  0x02U
#define SHPHLD_STATUS_OFFSET     0x14U

/* VBUS */
#define VBUS_OFFSET_DETECT       0x05U
#define VBUS_OFFSET_STATUS       0x07U

/* Charger */
#define CHGR_OFFSET_ERR_CLR      0x00U
#define CHGR_OFFSET_EN_SET       0x04U
#define CHGR_OFFSET_EN_CLR       0x05U
#define CHGR_OFFSET_DIS_SET      0x06U
#define CHGR_OFFSET_STATUS       0x34U

/* ADC */
#define ADC_OFFSET_NTCR_SEL      0x0AU
#define ADC_OFFSET_TASK_AUTO     0x0CU
#define ADC_OFFSET_RESULTS       0x10U

/* BUCK */
#define BUCK_OFFSET_ENA_CLR      0x01U
#define BUCK2_OFFSET_ENA_SET     0x02U
#define BUCK2_OFFSET_VOUT_NORM   0x0AU
#define BUCK_OFFSET_CTRL0        0x15U
#define BUCK2_OFFSET_PWM_SET     0x06U

/* GPIO */
#define GPIO_OFFSET_MODE         0x00U

/* LDSW / LDO */
#define LDSW_OFFSET_ENABLE       0x00U
#define LDSW_OFFSET_STATUS       0x04U
#define LDSW_OFFSET_LDOSEL       0x08U
#define LDSW_OFFSET_VOUTSEL      0x0CU

/* LED */
#define LED_OFFSET_MODE          0x00U

/* SHIPHOLD (ship-mode): nPM1300 datasheet SHIPHOLD task register */
#define MAIN_OFFSET_SHIP_TASK    0x01U   /* TASKENTERSHIPMODE */

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */
static bool              s_pmic_ready;
static uint8_t           s_cached_battery_pct;
static uint8_t           s_last_charger_status = 0xFFU;
static uint8_t           s_last_charger_error  = 0xFFU;
static uint8_t           s_last_ibat_stat      = 0xFFU;
static uint8_t           s_last_vbus_status;
static bool              s_vbus_ilim_applied;
static TimerHandle_t     s_charger_poll_timer;
static pmic_battery_cb_t s_battery_cb;

/* ------------------------------------------------------------------ */
/* Low-level I2C helpers                                               */
/* ------------------------------------------------------------------ */

/* nPM1300 uses a 2-byte address scheme: [base, offset] before data   */

static esp_err_t npm1300_reg_write(uint8_t base, uint8_t offset, uint8_t val)
{
    uint8_t buf[3] = { base, offset, val };
    return i2c_master_write_to_device(PMIC_I2C_PORT, PMIC_I2C_ADDR,
                                      buf, sizeof(buf),
                                      pdMS_TO_TICKS(10));
}

static esp_err_t npm1300_reg_read(uint8_t base, uint8_t offset, uint8_t *val)
{
    uint8_t addr[2] = { base, offset };
    return i2c_master_write_read_device(PMIC_I2C_PORT, PMIC_I2C_ADDR,
                                        addr, sizeof(addr),
                                        val, 1,
                                        pdMS_TO_TICKS(10));
}

static esp_err_t npm1300_reg_burst_read(uint8_t base, uint8_t offset,
                                        uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { base, offset };
    return i2c_master_write_read_device(PMIC_I2C_PORT, PMIC_I2C_ADDR,
                                        addr, sizeof(addr),
                                        buf, len,
                                        pdMS_TO_TICKS(20));
}

/* ------------------------------------------------------------------ */
/* Write-verify with retry (up to 3 attempts)                          */
/* ------------------------------------------------------------------ */
static esp_err_t pmic_write_verify_retry(const char *tag,
                                         uint8_t base, uint8_t offset,
                                         uint8_t expected, uint8_t mask)
{
    esp_err_t ret;
    uint8_t got;

    for (int i = 0; i < 3; i++) {
        ret = npm1300_reg_write(base, offset, expected);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s write try %d: %s", tag, i, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        ret = npm1300_reg_read(base, offset, &got);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "%s readback try %d: %s", tag, i, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if ((got & mask) == (expected & mask)) {
            if (i > 0) { ESP_LOGI(TAG, "%s OK after %d retry", tag, i); }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s mismatch try %d: wrote=0x%02X got=0x%02X (mask=0x%02X)",
                 tag, i, expected, got, mask);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(TAG, "%s give up after 3 tries", tag);
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Battery voltage → percentage (3000–4200 mV linear)                 */
/* ------------------------------------------------------------------ */
static uint8_t voltage_to_pct(int32_t mv)
{
    if (mv >= 4200) { return 100; }
    if (mv <= 3000) { return 0; }
    return (uint8_t)((mv - 3000) * 100 / 1200);
}

/* ------------------------------------------------------------------ */
/* ADC result struct (matches nPM1300 burst-read layout)               */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t ibat_stat;
    uint8_t msb_vbat;
    uint8_t msb_ntc;
    uint8_t msb_die;
    uint8_t msb_vsys;
    uint8_t lsb_a;
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t msb_ibat;
    uint8_t msb_vbus;
    uint8_t lsb_b;
} __attribute__((packed)) npm1300_adc_results_t;

static const char *ibat_stat_str(uint8_t stat)
{
    switch (stat) {
    case 0x04U: return "discharge";
    case 0x0CU: return "charge_trickle";
    case 0x0DU: return "charge_cool";
    case 0x0FU: return "charge_normal";
    default:    return "idle/other";
    }
}

/* ------------------------------------------------------------------ */
/* Charger poll (called from FreeRTOS timer, not ISR)                  */
/* ------------------------------------------------------------------ */
static void charger_poll(bool force)
{
    if (!s_pmic_ready) { return; }

    npm1300_adc_results_t adc = {0};
    uint8_t status = 0;
    uint8_t error  = 0;
    uint8_t vbus_status = 0;

    /* Read ADC results burst */
    npm1300_reg_burst_read(ADC_BASE, ADC_OFFSET_RESULTS,
                           (uint8_t *)&adc, sizeof(adc));

    /* Charger status / error */
    npm1300_reg_read(CHGR_BASE, CHGR_OFFSET_STATUS, &status);

    /* VBUS status */
    npm1300_reg_read(VBUS_BASE, VBUS_OFFSET_STATUS, &vbus_status);

    /* Reconstruct battery voltage from 10-bit ADC:
     *   VBAT_ADC = (msb_vbat<<2) | (lsb_a>>6)
     *   Vbat_mV  = VBAT_ADC * 5000 / 1023    (full-scale = 5 V) */
    uint16_t vbat_raw = ((uint16_t)adc.msb_vbat << 2) | (adc.lsb_a >> 6);
    int32_t  vbat_mv  = (int32_t)vbat_raw * 5000 / 1023;

    /* Average current (ibat): 10-bit, full-scale ±600 mA, signed */
    uint16_t ibat_raw = ((uint16_t)adc.msb_ibat << 2) | ((adc.lsb_b >> 4) & 0x03U);
    int32_t  ibat_ma  = ((int32_t)ibat_raw - 512) * 600 / 512;

    bool changed = force
        || (status != s_last_charger_status)
        || (error  != s_last_charger_error)
        || (adc.ibat_stat != s_last_ibat_stat)
        || (((vbus_status ^ s_last_vbus_status) & 0x01U) != 0U);

    if (changed) {
        ESP_LOGI(TAG, "bat=%ldmV %ldmA %s vbus=0x%02X err=0x%02X",
                 (long)vbat_mv, (long)ibat_ma,
                 ibat_stat_str(adc.ibat_stat), vbus_status, error);
    }

    s_last_charger_status = status;
    s_last_charger_error  = error;
    s_last_ibat_stat      = adc.ibat_stat;
    s_last_vbus_status    = vbus_status;

    /* Update cached battery percentage */
    uint8_t pct = voltage_to_pct(vbat_mv);
    s_cached_battery_pct = pct;
    if (s_battery_cb) { s_battery_cb(pct); }

    /* Re-apply VBUS current limit if VBUS just appeared */
    if ((vbus_status & 0x01U) != 0U) {
        if (!s_vbus_ilim_applied) {
            npm1300_reg_write(CHGR_BASE, CHGR_OFFSET_ERR_CLR, 1U);
            npm1300_reg_write(CHGR_BASE, CHGR_OFFSET_EN_SET,  1U);
            s_vbus_ilim_applied = true;
            ESP_LOGI(TAG, "VBUS detected: charger re-enabled");
        }
    } else {
        s_vbus_ilim_applied = false;
    }
}

static void charger_poll_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
#if PMIC_DIAG_LOG
    charger_poll(true);
    xTimerChangePeriod(s_charger_poll_timer, pdMS_TO_TICKS(1000), 0);
#else
    charger_poll(false);
    xTimerChangePeriod(s_charger_poll_timer, pdMS_TO_TICKS(2000), 0);
#endif
}

/* ------------------------------------------------------------------ */
/* PMIC_INT GPIO interrupt handler                                     */
/* ------------------------------------------------------------------ */
static void IRAM_ATTR pmic_int_isr(void *arg)
{
    (void)arg;
    /* Kick the charger-poll timer immediately from the ISR.
     * xTimerChangePeriodFromISR with period=1 tick schedules it
     * to fire as soon as the timer task runs. */
    BaseType_t woken = pdFALSE;
    xTimerChangePeriodFromISR(s_charger_poll_timer, 1, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------ */
/* Disable NTC monitoring (NTC pin tied to GND on this board)          */
/* ------------------------------------------------------------------ */
static esp_err_t disable_ntc(void)
{
    esp_err_t ret;

    ret = npm1300_reg_write(CHGR_BASE, CHGR_OFFSET_EN_CLR, 1U);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "charger disable failed"); return ret; }

    ret = npm1300_reg_write(CHGR_BASE, CHGR_OFFSET_DIS_SET, 2U); /* bit1 = NTC disable */
    if (ret != ESP_OK) { ESP_LOGE(TAG, "DIS_SET NTC disable failed"); return ret; }

    ret = npm1300_reg_write(ADC_BASE, ADC_OFFSET_NTCR_SEL, 0U);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "NTCR_SEL write failed"); return ret; }

    ret = npm1300_reg_write(ADC_BASE, ADC_OFFSET_TASK_AUTO, 1U); /* keep die-temp on */
    if (ret != ESP_OK) { ESP_LOGE(TAG, "auto-temp enable failed"); return ret; }

    npm1300_reg_write(CHGR_BASE, CHGR_OFFSET_ERR_CLR, 1U);
    npm1300_reg_write(CHGR_BASE, CHGR_OFFSET_EN_SET,  1U);

    ESP_LOGI(TAG, "NTC monitoring disabled, charger restarted");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public: reconfigure (idempotent)                                    */
/* ------------------------------------------------------------------ */
esp_err_t pmic_npm1300_reconfigure(void)
{
    esp_err_t ret;
    uint8_t   val;

    if (!s_pmic_ready) { return ESP_ERR_INVALID_STATE; }

    ESP_LOGI(TAG, "configuring registers...");

    /* Disable BUCK1 (not used on this board) */
    npm1300_reg_write(BUCK_BASE, BUCK_OFFSET_ENA_CLR, 0x01U);
    ESP_LOGI(TAG, "BUCK1 disabled");

    /* BUCK2 → 3.3 V (0x17 = (3300-1000)/100) before enabling */
    npm1300_reg_write(BUCK_BASE, BUCK2_OFFSET_VOUT_NORM, 0x17U);
    npm1300_reg_write(BUCK_BASE, BUCK2_OFFSET_ENA_SET,   0x01U);
    ESP_LOGI(TAG, "BUCK2 enabled at 3300 mV");

    /* Force BUCK2 into PWM mode (lower ripple → better PSRR for LDO1/ADS1298) */
    ret = npm1300_reg_read(BUCK_BASE, BUCK_OFFSET_CTRL0, &val);
    if (ret == ESP_OK) {
        npm1300_reg_write(BUCK_BASE, BUCK_OFFSET_CTRL0,   val & ~(uint8_t)0x02U);
        npm1300_reg_write(BUCK_BASE, BUCK2_OFFSET_PWM_SET, 0x01U);
        ESP_LOGI(TAG, "BUCK2 forced PWM");
    }

    /* GPIO0 MODE = 0x05 → PMIC interrupt output to MCU */
    pmic_write_verify_retry("GPIO0_MODE",  GPIO_BASE, GPIO_OFFSET_MODE,  0x05U, 0xFFU);

    /* LDSW LDOSEL: bit0=1 → LDO1 in LDO (regulated) mode */
    pmic_write_verify_retry("LDSW_LDOSEL", LDSW_BASE, LDSW_OFFSET_LDOSEL, 0x01U, 0x03U);

    /* LDO1 VOUTSEL = 0x17 → 3300 mV (powers ADS1298 AVDD) */
    pmic_write_verify_retry("LDO1_VOUTSEL", LDSW_BASE, LDSW_OFFSET_VOUTSEL, 0x17U, 0xFFU);

    /* Enable LDO1 via TASKLDSW1SET */
    ret = npm1300_reg_write(LDSW_BASE, LDSW_OFFSET_ENABLE, 0x01U);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO1 enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "LDO1 enabled (3.3 V for ADS1298)");

    /* Disable NTC, re-enable charger */
    disable_ntc();

    /* ---- Configuration readback ---- */
    ESP_LOGI(TAG, "===== PMIC Configuration Dump =====");

    if (npm1300_reg_read(GPIO_BASE, GPIO_OFFSET_MODE, &val) == ESP_OK) {
        ESP_LOGI(TAG, "GPIO0 MODE = 0x%02X (%s)", val,
                 (val == 0x05) ? "IRQ output (OK)" : "unexpected");
    }

    if (npm1300_reg_read(LDSW_BASE, LDSW_OFFSET_STATUS, &val) == ESP_OK) {
        ESP_LOGI(TAG, "LDSW STATUS = 0x%02X (LDO1 %s, LDO2 %s)", val,
                 (val & 0x03U) ? "ON" : "OFF",
                 (val & 0x0CU) ? "ON" : "OFF");
    }

    if (npm1300_reg_read(LDSW_BASE, LDSW_OFFSET_LDOSEL, &val) == ESP_OK) {
        ESP_LOGI(TAG, "LDSW LDOSEL = 0x%02X (LDO1=%s)", val,
                 (val & 0x01U) ? "LDO" : "LDSW");
    }

    if (npm1300_reg_read(LDSW_BASE, LDSW_OFFSET_VOUTSEL, &val) == ESP_OK) {
        ESP_LOGI(TAG, "LDO1 VOUTSEL = 0x%02X (%u mV%s)", val,
                 (unsigned)(1000U + val * 100U),
                 (val == 0x17) ? " OK" : " MISMATCH");
    }

    if (npm1300_reg_read(ADC_BASE, ADC_OFFSET_NTCR_SEL, &val) == ESP_OK) {
        ESP_LOGI(TAG, "NTC NTCR_SEL = 0x%02X (%s)", val,
                 (val == 0) ? "disabled OK" : "still enabled");
    }

    ESP_LOGI(TAG, "===== PMIC Config Dump Done =====");

    /* Kick charger poll immediately */
    charger_poll(true);
    xTimerChangePeriod(s_charger_poll_timer, pdMS_TO_TICKS(2000), 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public: init                                                        */
/* ------------------------------------------------------------------ */
esp_err_t pmic_npm1300_init(void)
{
    ESP_LOGI(TAG, "init start (SDA=%d SCL=%d INT=%d I2C%d addr=0x%02X)",
             PMIC_I2C_SDA, PMIC_I2C_SCL, PMIC_INT_PIN,
             PMIC_I2C_PORT, PMIC_I2C_ADDR);

    /* ---- Install I2C bus ---- */
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PMIC_I2C_SDA,
        .scl_io_num       = PMIC_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_DISABLE, /* R34 external pull-up */
        .scl_pullup_en    = GPIO_PULLUP_DISABLE, /* R33 external pull-up */
        .master.clk_speed = PMIC_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_param_config(PMIC_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = i2c_driver_install(PMIC_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C%d ready (%d Hz)", PMIC_I2C_PORT, PMIC_I2C_FREQ_HZ);

    /* ---- Verify the PMIC is alive by reading GPIO0 mode ---- */
    uint8_t probe = 0;
    ret = npm1300_reg_read(GPIO_BASE, GPIO_OFFSET_MODE, &probe);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PMIC probe read failed: %s (check wiring / address)",
                 esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PMIC probe OK (GPIO0 MODE=0x%02X)", probe);
    s_pmic_ready = true;

    /* ---- Create charger-poll timer (not started yet) ---- */
    s_charger_poll_timer = xTimerCreate("pmic_poll",
                                        pdMS_TO_TICKS(2000),
                                        pdFALSE,       /* one-shot; re-arms in callback */
                                        NULL,
                                        charger_poll_timer_cb);
    if (!s_charger_poll_timer) {
        ESP_LOGE(TAG, "timer create failed");
        return ESP_ERR_NO_MEM;
    }

    /* ---- Configure PMIC_INT GPIO (active-low, falling edge) ---- */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PMIC_INT_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PMIC_INT_PIN, pmic_int_isr, NULL);

    /* ---- Apply full register configuration ---- */
    return pmic_npm1300_reconfigure();
}

/* ------------------------------------------------------------------ */
/* Public: accessors                                                   */
/* ------------------------------------------------------------------ */
uint8_t pmic_npm1300_get_battery_pct(void)
{
    return s_cached_battery_pct;
}

pmic_charge_state_t pmic_npm1300_get_charge_state(void)
{
    if ((s_last_vbus_status & 0x01U) == 0U) {
        return PMIC_CHARGE_NONE;
    }
    if (s_last_charger_status != 0xFFU &&
        (s_last_charger_status & 0x02U) != 0U) {
        return PMIC_CHARGE_COMPLETE;
    }
    return PMIC_CHARGE_ACTIVE;
}

esp_err_t pmic_npm1300_poll_shphld(bool *pressed, bool *released)
{
    if (pressed)  { *pressed  = false; }
    if (released) { *released = false; }

    if (!s_pmic_ready) { return ESP_ERR_INVALID_STATE; }

    uint8_t evt = 0;
    esp_err_t ret = npm1300_reg_read(MAIN_EVT_BASE, SHPHLD_EVT_OFFSET_SET, &evt);
    if (ret != ESP_OK) { return ret; }

    if (pressed)  { *pressed  = (evt & SHPHLD_EVT_PRESS_MASK)   != 0; }
    if (released) { *released = (evt & SHPHLD_EVT_RELEASE_MASK) != 0; }

    if (evt & (SHPHLD_EVT_PRESS_MASK | SHPHLD_EVT_RELEASE_MASK)) {
        npm1300_reg_write(MAIN_EVT_BASE, SHPHLD_EVT_OFFSET_CLR,
                          evt & (SHPHLD_EVT_PRESS_MASK | SHPHLD_EVT_RELEASE_MASK));
    }
    return ESP_OK;
}

bool pmic_npm1300_is_shphld_pressed(void)
{
    if (!s_pmic_ready) { return false; }

    uint8_t status = 0;
    esp_err_t ret = npm1300_reg_read(MAIN_EVT_BASE, SHPHLD_STATUS_OFFSET, &status);
    if (ret != ESP_OK) { return false; }
    return (status & 0x01U) != 0;
}

void pmic_npm1300_set_battery_cb(pmic_battery_cb_t cb)
{
    s_battery_cb = cb;
}

/* ------------------------------------------------------------------ */
/* Ship mode (power-off)                                               */
/* ------------------------------------------------------------------ */
esp_err_t pmic_npm1300_enter_ship_mode(void)
{
    if (!s_pmic_ready) { return ESP_ERR_INVALID_STATE; }

    ESP_LOGW(TAG, "Entering nPM1300 ship mode (MCU will lose power)");

    /* Clear RELEASE event; keep PRESS event for boot-guard after wake */
    npm1300_reg_write(MAIN_EVT_BASE, SHPHLD_EVT_OFFSET_CLR, SHPHLD_EVT_RELEASE_MASK);

    vTaskDelay(pdMS_TO_TICKS(20)); /* allow UART to flush */

    /* nPM1300 TASKENTERSHIPMODE register: base=MAIN_EVT_BASE, offset=0x01 */
    esp_err_t ret = npm1300_reg_write(MAIN_EVT_BASE, MAIN_OFFSET_SHIP_TASK, 1U);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ship mode write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Should not reach here under normal conditions */
    vTaskDelay(pdMS_TO_TICKS(500));
    return ESP_OK;
}

bool pmic_npm1300_is_shphld_wakeup(void)
{
    if (!s_pmic_ready) { return false; }

    uint8_t evt = 0;
    esp_err_t ret = npm1300_reg_read(MAIN_EVT_BASE, SHPHLD_EVT_OFFSET_SET, &evt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "is_shphld_wakeup: read failed %s", esp_err_to_name(ret));
        return false;
    }

    bool press = (evt & SHPHLD_EVT_PRESS_MASK) != 0;
    if (press) {
        npm1300_reg_write(MAIN_EVT_BASE, SHPHLD_EVT_OFFSET_CLR, SHPHLD_EVT_PRESS_MASK);
    }
    return press;
}
