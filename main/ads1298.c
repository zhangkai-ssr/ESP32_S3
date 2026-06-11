#include "ads1298.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

static const char *TAG = "ads1298";

static spi_device_handle_t s_spi;
static SemaphoreHandle_t   s_drdy_sem;
static volatile uint32_t   s_drdy_isr_count;
static volatile uint32_t   s_drdy_take_count;
static volatile uint32_t   s_drdy_overflow_count;

/* ---- DRDY interrupt ---- */

static void IRAM_ATTR drdy_isr_handler(void *arg)
{
    s_drdy_isr_count++;
    BaseType_t woken = pdFALSE;
    if (uxSemaphoreGetCountFromISR(s_drdy_sem) < 32) {
        xSemaphoreGiveFromISR(s_drdy_sem, &woken);
    } else {
        s_drdy_overflow_count++;
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* ---- SPI helpers ---- */

static inline void cs_assert(void)
{
    gpio_set_level(ADS1298_PIN_CS, 0);
    esp_rom_delay_us(1);
}

static inline void cs_deassert(void)
{
    esp_rom_delay_us(1);
    gpio_set_level(ADS1298_PIN_CS, 1);
}

/* DMA-safe static buffers; max transaction = 1 (cmd) + 54 (data) = 55 bytes */
#define SPI_BUF_SIZE  56
static uint8_t s_tx_buf[SPI_BUF_SIZE];
static uint8_t s_rx_buf[SPI_BUF_SIZE];

static esp_err_t spi_rw(const uint8_t *tx, uint8_t *rx, size_t len)
{
    memcpy(s_tx_buf, tx, len);
    memset(s_rx_buf, 0, len);

    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = s_tx_buf,
        .rx_buffer = s_rx_buf,
    };
    esp_err_t ret = spi_device_polling_transmit(s_spi, &t);
    if (ret == ESP_OK && rx) {
        memcpy(rx, s_rx_buf, len);
    }
    return ret;
}

/* ---- Public API ---- */

esp_err_t ads1298_send_command(uint8_t cmd)
{
    uint8_t tx = cmd;
    uint8_t rx = 0;
    cs_assert();
    esp_err_t ret = spi_rw(&tx, &rx, 1);
    cs_deassert();
    return ret;
}

esp_err_t ads1298_read_reg(uint8_t reg, uint8_t *val)
{
    uint8_t tx[3] = { ADS1298_CMD_RREG | (reg & 0x1F), 0x00, 0x00 };
    uint8_t rx[3] = {0};
    cs_assert();
    esp_err_t ret = spi_rw(tx, rx, 3);
    cs_deassert();
    if (ret == ESP_OK) {
        *val = rx[2];
    }
    return ret;
}

esp_err_t ads1298_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t tx[3] = { ADS1298_CMD_WREG | (reg & 0x1F), 0x00, val };
    cs_assert();
    esp_err_t ret = spi_rw(tx, NULL, 3);
    cs_deassert();
    return ret;
}

esp_err_t ads1298_write_regs(uint8_t reg, const uint8_t *vals, uint8_t count)
{
    uint8_t tx_buf[27];
    tx_buf[0] = ADS1298_CMD_WREG | (reg & 0x1F);
    tx_buf[1] = count - 1;
    memcpy(&tx_buf[2], vals, count);
    cs_assert();
    esp_err_t ret = spi_rw(tx_buf, NULL, 2 + count);
    cs_deassert();
    return ret;
}

esp_err_t ads1298_read_id(uint8_t *id)
{
    return ads1298_read_reg(ADS1298_REG_ID, id);
}

esp_err_t ads1298_read_data(uint8_t *buf, size_t len)
{
    if (len < ADS1298_TOTAL_DATA_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    /* RDATAC mode: assert CS when DRDY low, clock out data directly (no command byte).
     *
     * NOTE on daisy-chain dead bit:
     * ADS1298 inserts 1 'don't care' bit between each device's 216-bit data
     * frame. Compensating on the firmware side by reading an extra byte and
     * shifting introduced a race condition (likely SPI/DRDY timing margin
     * shrinking) that cut throughput in half and corrupted 18% of packets.
     *
     * Decision: keep the firmware reading 54 bytes as before (stable 2000 pkt/s),
     * apply the 1-bit left-shift to chip B data on the host side in
     * align_streams.py / parsers. The dead bit then manifests as chip B
     * status reading 0x60 instead of 0xC0 in the raw stream, which the host
     * tools handle uniformly. */
    uint8_t tx[ADS1298_TOTAL_DATA_SIZE] = {0};
    cs_assert();
    esp_err_t ret = spi_rw(tx, buf, ADS1298_TOTAL_DATA_SIZE);
    cs_deassert();
    return ret;
}

esp_err_t ads1298_wait_drdy(uint32_t timeout_ms)
{
    if (xSemaphoreTake(s_drdy_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        s_drdy_take_count++;
        return ESP_OK;
    }
    /* ISR semaphore timed out - log GPIO level for diagnosis, then poll directly */
    ESP_LOGW(TAG, "DRDY sem timeout: GPIO%d level=%d  isr_count=%lu",
             ADS1298_PIN_DRDY, gpio_get_level(ADS1298_PIN_DRDY), s_drdy_isr_count);
    /* Polling fallback: handles wiring where DRDY fires but ISR is not set up correctly */
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (gpio_get_level(ADS1298_PIN_DRDY) == 0) {
            s_drdy_take_count++;
            return ESP_OK;
        }
        esp_rom_delay_us(50);
    }
    return ESP_ERR_TIMEOUT;
}

void ads1298_drdy_stats(uint32_t *isr, uint32_t *take, uint32_t *overflow)
{
    if (isr)      *isr      = s_drdy_isr_count;
    if (take)     *take     = s_drdy_take_count;
    if (overflow) *overflow = s_drdy_overflow_count;
}

float ads1298_get_pga_gain(int ch)
{
#if ADS1298_TEST_SIGNAL || ADS1298_INPUT_SHORT
    (void)ch;
    return 12.0f;
#else
    static const float s_pga_gain[8] = {
        12.0f, 12.0f, 12.0f, 12.0f,
        12.0f, 12.0f, 12.0f, 12.0f,
    };
    if (ch < 0 || ch >= 8) {
        return 12.0f;
    }
    return s_pga_gain[ch];
#endif
}

/* ---- Hardware reset sequence ---- */

static void ads1298_hw_reset(void)
{
    gpio_set_level(ADS1298_PIN_CS,    1);
    gpio_set_level(ADS1298_PIN_START, 0);
    gpio_set_level(ADS1298_PIN_RESET, 0);   /* Assert RESET (active-low) */

    vTaskDelay(pdMS_TO_TICKS(10));           /* Hold ≥ 2 tCLK; 10 ms is ample */

    gpio_set_level(ADS1298_PIN_RESET, 1);   /* Release RESET */
    vTaskDelay(pdMS_TO_TICKS(300));          /* Wait tPOR (internal clk ~128 ms; 300 ms margin) */

    ads1298_send_command(ADS1298_CMD_SDATAC);
    ads1298_send_command(ADS1298_CMD_STOP);
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ---- Start / Stop ---- */

esp_err_t ads1298_start_conversion(void)
{
    /* Per ADS1298 datasheet "Setting the Device for Basic Data Capture":
     * After writing CONFIG3 (PD_REFBUF=1) we MUST wait tWAIT_VCAP1 (typ 150 ms,
     * max 250 ms) for the internal reference buffer to stabilise. Without this
     * delay the ADC modulator never starts and DRDY stays high forever, even
     * though chip ID and register read-back work (those only need DVDD). */
    vTaskDelay(pdMS_TO_TICKS(250));

    ads1298_send_command(ADS1298_CMD_SDATAC);
    vTaskDelay(pdMS_TO_TICKS(5));

    /* Explicit WAKEUP in case a stale STANDBY survived the soft reset path. */
    ads1298_send_command(ADS1298_CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(5));

    xSemaphoreTake(s_drdy_sem, 0);             /* flush any stale counts */
    s_drdy_isr_count      = 0;
    s_drdy_take_count     = 0;
    s_drdy_overflow_count = 0;

    gpio_set_level(ADS1298_PIN_START, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    ads1298_send_command(ADS1298_CMD_START);
    vTaskDelay(pdMS_TO_TICKS(20));             /* let several conversions complete */
    ads1298_send_command(ADS1298_CMD_RDATAC);

    /* Sanity: by now isr_count should be >0 if hardware is healthy. */
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "start_conversion done; DRDY isr_count after 50 ms = %lu, "
                  "GPIO%d level = %d",
             (unsigned long)s_drdy_isr_count,
             ADS1298_PIN_DRDY, gpio_get_level(ADS1298_PIN_DRDY));
    return ESP_OK;
}

esp_err_t ads1298_stop_conversion(void)
{
    gpio_set_level(ADS1298_PIN_START, 0);
    return ESP_OK;
}

/* ---- Initialisation ---- */

esp_err_t ads1298_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "init start");

    /* Output GPIOs: CS (idle-high), RESET (active-low), START (idle-low) */
    const gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << ADS1298_PIN_CS) |
                        (1ULL << ADS1298_PIN_RESET) |
                        (1ULL << ADS1298_PIN_START),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);
    gpio_set_level(ADS1298_PIN_CS,    1);
    gpio_set_level(ADS1298_PIN_RESET, 0);
    gpio_set_level(ADS1298_PIN_START, 0);

    /* Input GPIO: DRDY (active-low, falling-edge interrupt) */
    const gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << ADS1298_PIN_DRDY),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&in_conf);

    /* DRDY semaphore (max depth 32 matches nRF original) */
    s_drdy_sem = xSemaphoreCreateCounting(32, 0);
    if (!s_drdy_sem) {
        ESP_LOGE(TAG, "failed to create DRDY semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* DRDY interrupt — install service only if not already installed */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_isr_handler_add(ADS1298_PIN_DRDY, drdy_isr_handler, NULL);
    ESP_LOGI(TAG, "DRDY interrupt enabled (falling edge, GPIO%d)", ADS1298_PIN_DRDY);

    /* SPI2 bus — no DMA to avoid alignment constraints on small transactions */
    const spi_bus_config_t buscfg = {
        .mosi_io_num     = ADS1298_PIN_MOSI,
        .miso_io_num     = ADS1298_PIN_MISO,
        .sclk_io_num     = ADS1298_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 1 + ADS1298_TOTAL_DATA_SIZE,
    };
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* SPI device: Mode 1 (CPOL=0, CPHA=1), CS driven manually */
    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,  /* 10 MHz (ADS1298 max: 20 MHz) */
        .mode           = 1,                   /* CPHA=1, CPOL=0 */
        .spics_io_num   = -1,                  /* manual CS */
        .queue_size     = 1,
        .flags          = 0,
    };
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ads1298_hw_reset();

    /* Read chip ID with retries */
    uint8_t id = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        ret = ads1298_read_id(&id);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "read ID failed (attempt %d): %s", attempt, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        ESP_LOGI(TAG, "chip ID = 0x%02X (attempt %d, expect 0xD2 or 0x92)", id, attempt);
        if (id == 0xD2 || id == 0x92) {
            break;
        }
        ESP_LOGW(TAG, "unexpected ID 0x%02X, retrying...", id);
        ads1298_send_command(ADS1298_CMD_SDATAC);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Register configuration — matching nRF54L15 reference:
     *   CONFIG1 = 0x84  HR mode, 2000 SPS, DAISY_EN=0 (daisy-chain active)
     *   CONFIG2 = 0x00  normal (no test signal)
     *   CONFIG3 = 0xCC  PD_REFBUF=1, RLDREF_INT=1, PD_RLD=1
     *   LOFF    = 0x00  lead-off detection disabled
     *   CHnSET  = 0x60  12x gain, normal electrode input
     *   RLD_SENSP/N = 0xFF  all 8 channels in RLD
     */
    uint8_t regs[25];
    regs[0x00] = 0x84;   /* CONFIG1: HR=1, DR=100 (2000 SPS) */
#if ADS1298_TEST_SIGNAL
    regs[0x01] = 0x10;   /* CONFIG2: INT_TEST=1 (internal 1Hz square wave) */
#else
    regs[0x01] = 0x00;   /* CONFIG2: normal mode */
#endif
    regs[0x02] = 0xCC;   /* CONFIG3: PD_REFBUF=1, RLDREF_INT=1, PD_RLD=1 */
    regs[0x03] = 0x00;   /* LOFF: disabled */
#if ADS1298_TEST_SIGNAL
    /* CHnSET MUX=101: internal test signal, GAIN=110 (12x) */
    regs[0x04] = 0x65;
    regs[0x05] = 0x65;
    regs[0x06] = 0x65;
    regs[0x07] = 0x65;
    regs[0x08] = 0x65;
    regs[0x09] = 0x65;
    regs[0x0A] = 0x65;
    regs[0x0B] = 0x65;
#elif ADS1298_INPUT_SHORT
    /* CHnSET MUX=001: input shorted, GAIN=110 (12x) */
    regs[0x04] = 0x61;
    regs[0x05] = 0x61;
    regs[0x06] = 0x61;
    regs[0x07] = 0x61;
    regs[0x08] = 0x61;
    regs[0x09] = 0x61;
    regs[0x0A] = 0x61;
    regs[0x0B] = 0x61;
#else
    /* CHnSET MUX=000: normal electrode input, GAIN=110 (12x) */
    regs[0x04] = 0x60;
    regs[0x05] = 0x60;
    regs[0x06] = 0x60;
    regs[0x07] = 0x60;
    regs[0x08] = 0x60;
    regs[0x09] = 0x60;
    regs[0x0A] = 0x60;
    regs[0x0B] = 0x60;
#endif
    regs[0x0C] = 0xFF;   /* RLD_SENSP: all 8ch in RLD */
    regs[0x0D] = 0xFF;   /* RLD_SENSN: all 8ch in RLD */
    regs[0x0E] = 0x00;
    regs[0x0F] = 0x00;
    regs[0x10] = 0x00;
    regs[0x11] = 0x00;
    regs[0x12] = 0x00;
    regs[0x13] = 0x00;
    regs[0x14] = 0x00;
    regs[0x15] = 0x00;
    regs[0x16] = 0x00;
    regs[0x17] = 0x00;
    regs[0x18] = 0x00;

    ret = ads1298_write_regs(ADS1298_REG_CONFIG1, regs, 25);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "register write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Readback verification */
    uint8_t cfg1 = 0, cfg2 = 0, cfg3 = 0, ch1 = 0;
    ads1298_read_reg(ADS1298_REG_CONFIG1, &cfg1);
    ads1298_read_reg(ADS1298_REG_CONFIG2, &cfg2);
    ads1298_read_reg(ADS1298_REG_CONFIG3, &cfg3);
    ads1298_read_reg(ADS1298_REG_CH1SET,  &ch1);
    ESP_LOGI(TAG, "readback: CONFIG1=0x%02X CONFIG2=0x%02X CONFIG3=0x%02X CH1SET=0x%02X",
             cfg1, cfg2, cfg3, ch1);

    uint8_t ch[8] = {0};
    uint8_t rld_p = 0, rld_n = 0;
    for (int i = 0; i < 8; i++) {
        ads1298_read_reg(ADS1298_REG_CH1SET + i, &ch[i]);
    }
    ads1298_read_reg(ADS1298_REG_RLD_SENSP, &rld_p);
    ads1298_read_reg(ADS1298_REG_RLD_SENSN, &rld_n);
    ESP_LOGI(TAG, "CHnSET: 1=%02X 2=%02X 3=%02X 4=%02X 5=%02X 6=%02X 7=%02X 8=%02X",
             ch[0], ch[1], ch[2], ch[3], ch[4], ch[5], ch[6], ch[7]);
    ESP_LOGI(TAG, "RLD_SENSP=0x%02X RLD_SENSN=0x%02X", rld_p, rld_n);

#if ADS1298_TEST_SIGNAL
    if (cfg1 != 0x84 || cfg2 != 0x10 || (cfg3 & 0xFE) != 0xCC) {
        ESP_LOGW(TAG, "CONFIG mismatch! got 0x%02X/0x%02X/0x%02X expected 0x84/0x10/0xCC", cfg1, cfg2, cfg3);
    }
    ESP_LOGI(TAG, "init done (2000Hz, 12x gain, TEST SIGNAL mode)");
#elif ADS1298_INPUT_SHORT
    if (cfg1 != 0x84 || cfg2 != 0x00 || (cfg3 & 0xFE) != 0xCC) {
        ESP_LOGW(TAG, "CONFIG mismatch! got 0x%02X/0x%02X/0x%02X expected 0x84/0x00/0xCC", cfg1, cfg2, cfg3);
    }
    ESP_LOGI(TAG, "init done (2000Hz, 12x gain, INPUT SHORT mode)");
#else
    if (cfg1 != 0x84 || cfg2 != 0x00 || (cfg3 & 0xFE) != 0xCC) {
        ESP_LOGW(TAG, "CONFIG mismatch! got 0x%02X/0x%02X/0x%02X expected 0x84/0x00/0xCC", cfg1, cfg2, cfg3);
    }
    ESP_LOGI(TAG, "init done (2000Hz, normal mode, gain: ch1-8=12x)");
#endif
    return ESP_OK;
}
