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
#include <string.h>
#include <stdio.h>

static const char *TAG = "npm1300_led";

/* ---- nPM1300 register pages ---- */
#define NPM1300_LED_PAGE        0x66    /* LED page (verified empirically) */
#define NPM1300_BUCK_PAGE       0x04    /* BUCK regulators */
#define NPM1300_LDSW_PAGE       0x08    /* LDSW / LDO regulators */
#define NPM1300_CHGR_PAGE       0x03    /* battery charger (BCHG) */
#define NPM1300_VBUS_PAGE       0x02    /* VBUS input limiter */
/* nPM1300 ADC sub-system lives at page 0x05 (per Zephyr mfd_npm1300).
 * The knowledge-base note that puts ADCNTCRSEL at page 0x02 is wrong on
 * this PMIC — we verified by reading VBAT through both bases and only the
 * page 0x05 read returns the real 3.98 V battery voltage. */
#define NPM1300_ADC_PAGE        0x05

/* ---- ADC page (0x05) offsets ---- */
#define NPM1300_ADC_TASK_VBAT   0x00    /* 1 = start VBAT measure  */
#define NPM1300_ADC_NTCR_SEL    0x0A    /* 0 = NTC pull-up disabled */
#define NPM1300_ADC_TASK_AUTO   0x0C    /* 1 = start auto-temp     */
#define NPM1300_ADC_VBAT_MSB    0x11    /* VBAT result high byte   */
#define NPM1300_ADC_NTC_MSB     0x12    /* NTC voltage result MSB  */
#define NPM1300_ADC_TEMP_MSB    0x13    /* die temperature MSB     */
#define NPM1300_ADC_VSYS_MSB    0x14    /* VSYS result MSB         */

/* ---- Charger page (0x03) offsets (per nPM1300 PS / Zephyr driver) ---- */
#define NPM1300_BCHG_ERR_CLR    0x00    /* write 1 = clear charger errors */
#define NPM1300_BCHG_EN_SET     0x04    /* write 1 = enable charger */
#define NPM1300_BCHG_EN_CLR     0x05    /* write 1 = disable charger */
#define NPM1300_BCHG_DIS_SET    0x06    /* bit1 = disable NTC monitoring */
#define NPM1300_BCHG_DIS_CLR    0x07    /* bit1 = re-enable NTC monitoring */
#define NPM1300_BCHG_ISETMSB    0x08    /* ICHG MSB: (mA/2) >> 1   */
#define NPM1300_BCHG_ISETLSB    0x09    /* ICHG LSB: (mA/2) &  1   */
#define NPM1300_BCHG_VTERM      0x0C    /* (V-3.50)/0.05 V steps   */
#define NPM1300_BCHG_VTERMR     0x0D    /* warm-temp term voltage  */
#define NPM1300_BCHG_STATUS     0x34    /* BCHGCHARGESTATUS        */
#define NPM1300_BCHG_ERR_REASON 0x36    /* BCHGERRREASON           */
#define NPM1300_BCHG_ERR_SENSOR 0x37    /* BCHGERRSENSOR           */

/* ---- VBUS page (0x02) offsets ---- */
#define NPM1300_VBUS_STATUS     0x07    /* bit0 = VBUS present     */
#define NPM1300_VBUS_TASKUPDILIM 0x00   /* write 1 = apply new ILIM */
#define NPM1300_VBUS_INILIM0    0x01    /* VBUS input current limit:
                                         *  0 = 100 mA (SDP unconfigured)
                                         *  1 = 500 mA (SDP configured)
                                         *  2..7 = higher (DCP / CDP) */

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
#define NPM1300_LDSW_STATUS     0x04    /* bit0-1=LDSW1, bit2-3=LDSW2 */
/* Per Zephyr regulator_npm1300.c: LDSW mode select is per-channel, with
 * channel offset added. So LDSW1 mode is at 0x08, LDSW2 mode at 0x09.
 * Each register is a SINGLE BIT (1=LDO mode, 0=LDSW mode). */
#define NPM1300_LDSW1_LDOSEL    0x08
#define NPM1300_LDSW2_LDOSEL    0x09
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

esp_err_t npm1300_enable_charger(void)
{
    esp_err_t ret;
    uint8_t   vbus_status = 0;

    ESP_LOGI(TAG, "configuring NPM1300 charger (150 mA, 4.15 V, NTC off)");

    /* Read VBUS status — informational only, charger setup still proceeds
     * because VBUS may come up after this function (e.g. battery-only boot). */
    uint8_t cmd[2] = { NPM1300_VBUS_PAGE, NPM1300_VBUS_STATUS };
    if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                     cmd, 2, &vbus_status, 1,
                                     pdMS_TO_TICKS(10)) == ESP_OK) {
        ESP_LOGI(TAG, "VBUS_STATUS = 0x%02X (VBUS %s)",
                 vbus_status, (vbus_status & 0x01) ? "PRESENT" : "ABSENT");
    }

    /* 0. Force VBUS input current limit to 500 mA. The nPM1300 hardware-
     *    default ILIM after VBUS insertion is 100 mA (SDP unconfigured) —
     *    the bump to 500 mA normally happens after USB host enumeration
     *    sets the SDP-configured state. When the board is powered by a
     *    plain USB charger (no enumeration) it stays at 100 mA, and the
     *    charger state machine refuses to leave idle because 100 mA cannot
     *    cover both VSYS draw and the 150 mA charge current we asked for. */
    ret = npm1300_write(NPM1300_VBUS_PAGE, NPM1300_VBUS_INILIM0, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "VBUS ILIM set failed: %s", esp_err_to_name(ret)); return ret; }
    ret = npm1300_write(NPM1300_VBUS_PAGE, NPM1300_VBUS_TASKUPDILIM, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "VBUS ILIM apply failed: %s", esp_err_to_name(ret)); return ret; }

    /* 1. Disable charger so we configure into a clean state machine. */
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_EN_CLR, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG disable failed: %s", esp_err_to_name(ret)); return ret; }

    /* 2. NTC config — battery pack (YJ503030) uses a 100 kΩ / β=4250 NTC.
     *    Explicitly re-enable NTC monitoring (BCHGDISABLECLR bit 1) in case
     *    an older firmware build left BCHGDISABLESET bit 1 latched. Then
     *    select the 100 kΩ pull-up and start the ADC auto-sample task so
     *    the charger sees a fresh VBAT/NTC reading.
     *    NTCR_SEL encoding: 0=off, 1=10k, 2=47k, 3=100k. */
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_DIS_CLR, 0x02);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG NTC re-enable failed: %s", esp_err_to_name(ret)); return ret; }
    ret = npm1300_write(NPM1300_ADC_PAGE,  NPM1300_ADC_NTCR_SEL, 0x03);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ADC NTCR_SEL failed: %s", esp_err_to_name(ret)); return ret; }
    ret = npm1300_write(NPM1300_ADC_PAGE,  NPM1300_ADC_TASK_AUTO, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "ADC TASK_AUTO failed: %s", esp_err_to_name(ret)); return ret; }

    /* 3. Charging current = 200 mA (0.5C, the standard charging current spec'd
     *    by the YJ503030 battery datasheet). Encoding per nPM1300 reg map:
     *      ICHG (mA) = MSB * 4 + LSB * 2
     *      200 = 50 * 4 + 0 * 2  → MSB = 50 (0x32), LSB = 0 */
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_ISETMSB, 50);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG ISETMSB failed: %s", esp_err_to_name(ret)); return ret; }
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_ISETLSB, 0);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG ISETLSB failed: %s", esp_err_to_name(ret)); return ret; }

    /* 4. Termination voltage = 4.20 V (= FC per battery datasheet, well below
     *    PCM over-charge threshold 4.28 V). Encoding: val = (V - 3.50) / 0.05.
     *      4.20 V → 14 (normal)
     *      4.00 V → 10 (warm JEITA) */
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_VTERM,  14);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG VTERM failed: %s", esp_err_to_name(ret)); return ret; }
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_VTERMR, 10);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG VTERMR failed: %s", esp_err_to_name(ret)); return ret; }

    /* 5. Let the state machine see the new config before we clear errors —
     *    some BCHG settings (VTERM, ICHG) are only sampled when leaving the
     *    disabled state.  100 ms is well over the spec'd internal settling. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 6. Clear any latched charger errors before re-enabling. */
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_ERR_CLR, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG ERR_CLR failed: %s", esp_err_to_name(ret)); return ret; }

    /* 7. Enable charger.  Task register: write 1 fires the task to enable. */
    ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_EN_SET, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "BCHG EN_SET failed: %s", esp_err_to_name(ret)); return ret; }

    /* 8. Allow the charger to settle, then re-pulse EN_SET — this matches the
     *    "disable → configure → enable" recipe in the knowledge base note and
     *    ensures the state machine actually picks up the new ICHG/VTERM. */
    vTaskDelay(pdMS_TO_TICKS(50));
    (void)npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_EN_SET, 0x01);

    /* 7. Read back every register we just programmed so we can spot any
     *    silent I2C-write loss or wrong-address bug from the log. */
    static const struct { uint8_t page, off; const char *name; } regs[] = {
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_ISETMSB, "ISETMSB" },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_ISETLSB, "ISETLSB" },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_VTERM,   "VTERM"   },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_VTERMR,  "VTERMR"  },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_DIS_SET, "DIS_SET" },
        { NPM1300_ADC_PAGE,  NPM1300_ADC_NTCR_SEL, "NTCR_SEL"},
    };
    for (int i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++) {
        uint8_t v = 0xFF;
        cmd[0] = regs[i].page; cmd[1] = regs[i].off;
        if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                         cmd, 2, &v, 1,
                                         pdMS_TO_TICKS(10)) == ESP_OK) {
            ESP_LOGI(TAG, "CHG_INIT: page=%02X off=%02X %-8s = 0x%02X",
                     regs[i].page, regs[i].off, regs[i].name, v);
        } else {
            ESP_LOGW(TAG, "CHG_INIT: read %02X/%02X (%s) failed",
                     regs[i].page, regs[i].off, regs[i].name);
        }
    }

    uint8_t status = 0;
    cmd[0] = NPM1300_CHGR_PAGE; cmd[1] = NPM1300_BCHG_STATUS;
    if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                     cmd, 2, &status, 1,
                                     pdMS_TO_TICKS(10)) == ESP_OK) {
        /* BCHGCHARGESTATUS bits: 0=battery_detected, 1=charging_complete,
         *   2=trickle, 3=cc, 4=cv, 5=recharge, 6=NTC_warm, 7=supplement */
        ESP_LOGI(TAG, "BCHG_STATUS = 0x%02X (bat=%d trickle=%d CC=%d CV=%d)",
                 status,
                 (status >> 0) & 1, (status >> 2) & 1,
                 (status >> 3) & 1, (status >> 4) & 1);
    }
    return ESP_OK;
}

void npm1300_log_charger_status(void)
{
    uint8_t cmd[2];
    uint8_t vbus = 0, bchg = 0;

    cmd[0] = NPM1300_VBUS_PAGE; cmd[1] = NPM1300_VBUS_STATUS;
    if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                     cmd, 2, &vbus, 1, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGW(TAG, "CHG: VBUS read failed");
        return;
    }
    cmd[0] = NPM1300_CHGR_PAGE; cmd[1] = NPM1300_BCHG_STATUS;
    if (i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                     cmd, 2, &bchg, 1, pdMS_TO_TICKS(10)) != ESP_OK) {
        ESP_LOGW(TAG, "CHG: BCHG_STATUS read failed");
        return;
    }
    /* BCHGCHARGESTATUS bits:
     *  0 = battery_detected
     *  1 = charging_complete
     *  2 = trickle_charge   (low cell → ~10 % ICHG)
     *  3 = constant_current (CC, full ICHG)
     *  4 = constant_voltage (CV, near termination V)
     *  5 = recharge
     *  6 = NTC warm/JEITA
     *  7 = supplement (battery sourcing into VSYS together with VBUS) */
    const char *phase =
        (bchg & 0x02) ? "complete"   :
        (bchg & 0x10) ? "CV"         :
        (bchg & 0x08) ? "CC"         :
        (bchg & 0x04) ? "trickle"    :
        (bchg & 0x20) ? "recharge"   :
        (bchg & 0x80) ? "supplement" :
                        "idle";
    uint8_t err_reason = 0, err_sensor = 0;
    cmd[0] = NPM1300_CHGR_PAGE; cmd[1] = NPM1300_BCHG_ERR_REASON;
    (void)i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                       cmd, 2, &err_reason, 1, pdMS_TO_TICKS(10));
    cmd[0] = NPM1300_CHGR_PAGE; cmd[1] = NPM1300_BCHG_ERR_SENSOR;
    (void)i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                       cmd, 2, &err_sensor, 1, pdMS_TO_TICKS(10));
    ESP_LOGI(TAG,
             "CHG: VBUS=%s BCHG=0x%02X [%s] bat_det=%d err_r=0x%02X err_s=0x%02X",
             (vbus & 0x01) ? "ON" : "OFF",
             bchg, phase, (bchg >> 0) & 1, err_reason, err_sensor);

    /* Dump every register we configured at init so we can see, from the
     * runtime log alone, whether each write actually stuck. The boot-time
     * dump is often lost to the host serial monitor sync delay. */
    static const struct { uint8_t page, off; const char *name; } regs[] = {
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_ISETMSB, "ISETMSB" },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_ISETLSB, "ISETLSB" },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_VTERM,   "VTERM"   },
        { NPM1300_CHGR_PAGE, NPM1300_BCHG_DIS_SET, "DIS_SET" },
        { NPM1300_ADC_PAGE,  NPM1300_ADC_NTCR_SEL, "NTCR_SEL"},
    };
    char line[160] = "CHG_REGS:";
    size_t off = strlen(line);
    for (int i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++) {
        uint8_t v = 0xFF;
        cmd[0] = regs[i].page; cmd[1] = regs[i].off;
        (void)i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                           cmd, 2, &v, 1, pdMS_TO_TICKS(10));
        off += snprintf(line + off, sizeof(line) - off,
                        " %s=0x%02X", regs[i].name, v);
    }
    ESP_LOGI(TAG, "%s", line);

    /* Read the most recent auto-measurement results.  We do NOT trigger a
     * fresh single-shot conversion here: the auto sequence (started in
     * enable_charger via TASK_AUTO) keeps fresh results in 0x11/0x13/0x14,
     * and writing TASK_VBAT from the main task was tripping the interrupt
     * watchdog on CPU0 (likely via an nPM1300 INT pulse that nobody handles).
     */
    uint8_t vbat_msb = 0, vsys_msb = 0, ntc_msb = 0;
    cmd[0] = NPM1300_ADC_PAGE; cmd[1] = NPM1300_ADC_VBAT_MSB;
    (void)i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                       cmd, 2, &vbat_msb, 1, pdMS_TO_TICKS(10));
    cmd[0] = NPM1300_ADC_PAGE; cmd[1] = NPM1300_ADC_VSYS_MSB;
    (void)i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                       cmd, 2, &vsys_msb, 1, pdMS_TO_TICKS(10));
    cmd[0] = NPM1300_ADC_PAGE; cmd[1] = NPM1300_ADC_NTC_MSB;
    (void)i2c_master_write_read_device(NPM1300_I2C_PORT, NPM1300_I2C_ADDR,
                                       cmd, 2, &ntc_msb, 1, pdMS_TO_TICKS(10));

    /* MSBs are top 8 bits of a 10-bit value (LSBs in 0x18 — ignored for
     * coarse view).  Convert as: V = vfull * msb / 255 (close enough). */
    unsigned vbat_mv = (5000u  * vbat_msb) / 255u;
    unsigned vsys_mv = (6375u  * vsys_msb) / 255u;
    unsigned ntc_mv  = (1000u  * ntc_msb)  / 255u;
    ESP_LOGI(TAG, "CHG_ADC: VBAT=%u mV  VSYS=%u mV  NTC=%u mV  (raw 0x%02X/0x%02X/0x%02X)",
             vbat_mv, vsys_mv, ntc_mv, vbat_msb, vsys_msb, ntc_msb);
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
    /* Mode select is PER-CHANNEL (two separate registers, each 1-bit):
     *   LDSW1 mode → 0x08, LDSW2 mode → 0x09
     *   1 = LDO (regulated), 0 = LDSW (load switch passthrough)
     * (Per Zephyr regulator_npm1300.c source.)
     */
    ret = npm1300_write_verify("LDO1_MODE", NPM1300_LDSW_PAGE,
                               NPM1300_LDSW1_LDOSEL, 0x01, 0x01);
    if (ret != ESP_OK) return ret;
    ret = npm1300_write_verify("LDO2_MODE", NPM1300_LDSW_PAGE,
                               NPM1300_LDSW2_LDOSEL, 0x01, 0x01);
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
        /* LDSW1_ON_MASK=0x03 (bits 0-1), LDSW2_ON_MASK=0x0C (bits 2-3) */
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
