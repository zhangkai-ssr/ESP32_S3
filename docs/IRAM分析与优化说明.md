# ESP32-S3 IRAM 分析与优化说明

> 日期：2026-06-04  
> 固件：ESP-IDF v5.3.1，目标芯片 ESP32-S3-PICO-N8R8

---

## 现象

`idf.py size` 输出中，IRAM 始终显示 99.99%（16383/16384 字节），仅剩 1 字节：

```
│ IRAM                │  16383 │ 99.99 │       1 │  16384 │
│    .text            │  15356 │ 93.73 │         │        │
│    .vectors         │   1027 │  6.27 │         │        │
```

---

## 根本原因：idf_size.py 的显示假象

### ESP32-S3 实际内存布局

| 物理内存 | 大小 | IRAM 地址 | DRAM 地址 |
|---|---|---|---|
| SRAM0 | 32KB | 0x40370000–0x40377FFF | 无（纯 IRAM） |
| SRAM1 | 128KB | 0x40378000–0x403BFFFF | 0x3FC88000–0x3FC…（DIRAM） |

- **I-cache** 占用 SRAM0 的前 16KB（`CONFIG_ESP32S3_INSTRUCTION_CACHE_16KB=y`）
- SRAM0 剩余 **16KB** 可作纯 IRAM（不可作 DRAM）

### Linker 实际定义

从 `build/ESP32-S3.map` 中可见 linker 定义的 IRAM 区域：

```
Name         Origin     Length     Attributes
iram0_0_seg  0x40374000 0x00057700 xr
```

**`iram0_0_seg` 实际大小 = 0x57700 = 349,056 字节（约 341 KB）**，是一块横跨 SRAM0 和 SRAM1 的**连续**区域。Linker 没有在 16KB 处设任何硬边界。

### idf_size.py 的拆分方式

`idf_size.py` 按物理内存属性将该连续区域人为拆开报告：

```
0x40374000 ── 0x40377FFF  (16KB, SRAM0 纯 IRAM)  → 报告为 "IRAM"
0x40378000 ────────────── (SRAM1, DRAM+IRAM 双映射) → 报告为 "DIRAM .text"
```

`.iram0.text` 段从 0x40374404 开始，连续铺满 SRAM0 的 16KB 窗口后自然流入 SRAM1。这是**正常行为**，不是溢出错误。`idf_size.py` 分开显示导致 SRAM0 那部分看起来"满了"，但 Linker 本身并不受此约束。

---

## 尝试过的优化及实际效果

### 验证过程

通过分析 map 文件，定位纯 IRAM（0x40374404–0x40377FFF）中各库的占用量：

| 库 | 纯 IRAM 占用 |
|---|---|
| libfreertos.a | ~1044 B |
| libesp_system.a | ~664 B |
| libesp_hw_support.a | ~572 B |
| libspi_flash.a | ~500 B |
| libhal.a | ~380 B |
| libheap.a | ~296 B |
| **libesp_wifi.a** | **仅 20 B** |

WiFi 代码在纯 IRAM 中只占 20 字节，绝大多数 WiFi `IRAM_ATTR` 代码实际落在 DIRAM 溢出区。

### 各配置的实际效果

| 配置项 | 目标 | 纯 IRAM 变化 | DIRAM .text 变化 |
|---|---|---|---|
| `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` | 移走 WiFi RX 代码 | **无变化** | 减少 |
| `CONFIG_ESP_WIFI_IRAM_OPT=n` | 移走 WiFi 主路径 | **无变化** | 减少 |
| `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` | FreeRTOS 非 ISR 函数进 flash | **无变化** | **-27,960 B ✓** |
| `CONFIG_SPI_MASTER_ISR_IN_IRAM=n` | SPI ISR 移出 IRAM | 极小 | 级联减少 |

**结论：纯 IRAM（15356 B）由 Linker 排布顺序决定，只要 `.iram0.text` 总量 > 16KB，SRAM0 窗口就会始终"满"。这一数字无法通过 Kconfig 降低，也不需要降低。**

---

## 实际内存健康状况（优化后）

| 指标 | 值 | 说明 |
|---|---|---|
| IRAM 区域总量（Linker） | 349 KB | `iram0_0_seg` 实际大小 |
| `.iram0.text` 当前使用 | ~72 KB | 15 KB（SRAM0）+ 57 KB（DIRAM） |
| **IRAM 真实使用率** | **~20%** | 远未饱和 |
| DIRAM 剩余 | 244,372 B（~238 KB） | 充足 |
| Flash Code 剩余 | 7,780,650 B（~7.4 MB） | 充足 |

**当前内存状况完全健康。**

---

## 当前 sdkconfig.defaults 配置说明

```ini
# ---- IRAM 释放 ----
# FreeRTOS 非 ISR 函数移到 flash，可减少约 28 KB 的 DIRAM .text 占用
CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y
# SPI master ISR 移出 IRAM；本项目用 polling transmit + SPI_DMA_DISABLED，ISR 从不触发
CONFIG_SPI_MASTER_ISR_IN_IRAM=n
# WiFi RX 路径移出 IRAM（减少 DIRAM .text 压力）
CONFIG_ESP_WIFI_RX_IRAM_OPT=n
```

这三条虽不能降低 `idf_size.py` 的"IRAM"行，但**有效释放了 DIRAM 压力，为未来功能扩展提供了更多空间**。

---

## 注意事项与未来风险

| 场景 | 风险 | 处理方式 |
|---|---|---|
| 加入 OTA（flash 写入期间 SPI 仍在采集） | `SPI_MASTER_ISR_IN_IRAM=n` 可能导致 SPI ISR 失效 | OTA 期间暂停 ADS1298 采集，或重新打开该项 |
| 加入运行期 NVS 写入 | FreeRTOS 函数在 flash 中，flash 擦写期间不能调用任务 API | 确保 NVS 写操作在任务上下文外完成 |
| I-cache 无 8KB 选项 | ESP32-S3 的 I-cache 只有 16KB/32KB 两档，无法通过缩小 cache 来扩大 IRAM | 不适用于本方案 |

---

## 关键结论

> **`idf.py size` 的"IRAM 99.99%"不是错误，也不是危险信号。**  
> 它是 `idf_size.py` 对 ESP32-S3 内存拓扑的拆分展示方式所导致的正常现象。  
> 实际 IRAM 区域为 349KB，当前仅使用约 20%，系统内存充裕。
