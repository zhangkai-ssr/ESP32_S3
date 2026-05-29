#ifndef ADS1298_H_
#define ADS1298_H_

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ---- GPIO pin assignments (from hardware netlist SHTB-V01) ---- */
#define ADS1298_PIN_SCLK    7
#define ADS1298_PIN_MOSI    13
#define ADS1298_PIN_MISO    40
#define ADS1298_PIN_CS      8
#define ADS1298_PIN_RESET   6
#define ADS1298_PIN_START   12
#define ADS1298_PIN_DRDY    14

/* ---- SPI command opcodes ---- */
#define ADS1298_CMD_WAKEUP   0x02
#define ADS1298_CMD_STANDBY  0x04
#define ADS1298_CMD_RESET    0x06
#define ADS1298_CMD_START    0x08
#define ADS1298_CMD_STOP     0x0A
#define ADS1298_CMD_RDATAC   0x10
#define ADS1298_CMD_SDATAC   0x11
#define ADS1298_CMD_RDATA    0x12
#define ADS1298_CMD_RREG     0x20
#define ADS1298_CMD_WREG     0x40

/* ---- Register addresses ---- */
#define ADS1298_REG_ID          0x00
#define ADS1298_REG_CONFIG1     0x01
#define ADS1298_REG_CONFIG2     0x02
#define ADS1298_REG_CONFIG3     0x03
#define ADS1298_REG_LOFF        0x04
#define ADS1298_REG_CH1SET      0x05
#define ADS1298_REG_CH2SET      0x06
#define ADS1298_REG_CH3SET      0x07
#define ADS1298_REG_CH4SET      0x08
#define ADS1298_REG_CH5SET      0x09
#define ADS1298_REG_CH6SET      0x0A
#define ADS1298_REG_CH7SET      0x0B
#define ADS1298_REG_CH8SET      0x0C
#define ADS1298_REG_RLD_SENSP   0x0D
#define ADS1298_REG_RLD_SENSN   0x0E
#define ADS1298_REG_LOFF_SENSP  0x0F
#define ADS1298_REG_LOFF_SENSN  0x10
#define ADS1298_REG_LOFF_FLIP   0x11
#define ADS1298_REG_LOFF_STATP  0x12
#define ADS1298_REG_LOFF_STATN  0x13
#define ADS1298_REG_GPIO        0x14
#define ADS1298_REG_PACE        0x15
#define ADS1298_REG_RESP        0x16
#define ADS1298_REG_CONFIG4     0x17
#define ADS1298_REG_WCT1        0x18
#define ADS1298_REG_WCT2        0x19

/* 27 bytes per RDATA frame: 3 status + 8ch * 3 bytes.
 * Hardware has 2 ADS1298 in daisy-chain (U3 upstream → U17 downstream → ESP32).
 * One read yields 54 bytes: [27 bytes U17 data][27 bytes U3 data]. */
#define ADS1298_DATA_FRAME_SIZE   27
#define ADS1298_CHAIN_DEVICES     2
#define ADS1298_TOTAL_DATA_SIZE   (ADS1298_DATA_FRAME_SIZE * ADS1298_CHAIN_DEVICES)

/* ---- Mode switch: 0 = normal electrode input, 1 = internal test signal ----
 * 置 1 后, 全部 8 通道切到芯片内部 1Hz / 1mVpp 方波 (CHnSET MUX=101, CONFIG2 INT_TEST=1).
 * 用途: 验证 ADS1298 + 电源 + SPI 链路是否健康.
 * 调试完务必改回 0. */
#ifndef ADS1298_TEST_SIGNAL
#define ADS1298_TEST_SIGNAL  0
#endif

/* ---- Debug: 0 = normal, 1 = input shorted (MUX=001) ----
 * 置 1 后, 全部通道把输入端在 PGA 内部短接到 (VREFP+VREFN)/2, 看 ADC 自身噪声底.
 * 与 ADS1298_TEST_SIGNAL 互斥, 后者优先. */
#ifndef ADS1298_INPUT_SHORT
#define ADS1298_INPUT_SHORT  0
#endif

esp_err_t ads1298_init(void);
esp_err_t ads1298_send_command(uint8_t cmd);
esp_err_t ads1298_read_reg(uint8_t reg, uint8_t *val);
esp_err_t ads1298_write_reg(uint8_t reg, uint8_t val);
esp_err_t ads1298_write_regs(uint8_t reg, const uint8_t *vals, uint8_t count);
esp_err_t ads1298_read_id(uint8_t *id);
esp_err_t ads1298_start_conversion(void);
esp_err_t ads1298_stop_conversion(void);

/* Read one conversion frame (ADS1298_TOTAL_DATA_SIZE bytes) via RDATA command.
 * buf must be at least ADS1298_TOTAL_DATA_SIZE bytes. */
esp_err_t ads1298_read_data(uint8_t *buf, size_t len);

/* Wait for DRDY falling edge. Returns ESP_OK or ESP_ERR_TIMEOUT. */
esp_err_t ads1298_wait_drdy(uint32_t timeout_ms);

/* 返回通道 ch (0..7) 的 PGA 增益倍数.
 * 当前 normal 配置: ch1..ch8 = 12x. */
float ads1298_get_pga_gain(int ch);

/* DRDY 诊断: ISR 总次数 / 线程实际取数次数 / sem overflow 丢点次数. */
void ads1298_drdy_stats(uint32_t *isr, uint32_t *take, uint32_t *overflow);

#endif /* ADS1298_H_ */
