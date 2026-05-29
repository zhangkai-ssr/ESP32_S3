# Hardware

**状态：开发中**（参数可能调整，以固件 / 嵌入式团队最新版本为准）
**同步**：2026-05-20（按当前 `prj.conf` + DTS overlay + `src/` 实际行为整理）

> 本文档中各项均给出**当前状态**：
> - "**已实现**" — 固件已落地并可验证（注明来源文件）
> - "**未实现**" — 当前固件还没做，App 同学按"目前没有"对待，待后续版本演进
> - "**待产品确认**" — 取决于产品决策，与固件无关

变更摘要见同目录 [`docs-changes-2026-05-12.md`](./docs-changes-2026-05-12.md)。

---

## 1. 外观与结构

渲染图见 [`../hardware/`](../hardware/)：`device1.png` · `device2.png` · `device3.png` · `device4.png`

可见特征：

- **EMG 电极**：8 颗围绕手腕内侧沿表带均匀分布（与 Meta Neural Band 同路线）
- **表体顶面**：一个主实体按钮 + LED 指示孔
- **表体侧面**：**实际为 1 颗 RGB LED**（BOM 中 `LED1 = XL-1010RGB-YG`，由 nPM1300 内置 LED 驱动驱动 R/G/B 三通道）。渲染图上的 3 颗 LED 仅为外观示意，最终方案以 BOM 为准。
- **表带**：编织布面，磁吸或搭扣收尾
- **无显示屏**：设备本身零 UI，所有交互都发生在手机 App 上

---

## 2. 传感器

| 传感器 | 参数 |
|---|---|
| EMG | 8 通道, 1000 Hz, 12-bit 有符号 (ADS1298, ±2048 μV) |
| IMU | ICM-42670-P, 200 Hz, 加速度 ±4 g + 陀螺 ±500 dps + 四元数（设备端 Fusion AHRS 融合） |

> IMU 量程定义见 `src/imu.c:60-73`，App 端 6 轴原始数据按 ±4 g / ±500 dps 解释 (int16 满量程 32768)。

---

## 3. 电源 / 电池 / 充电

> 来源：`boards/nrf54l15dk_nrf54l15_cpuapp.overlay` (`pmic_charger` 节点) + `src/pmic_npm1300.c` + `src/system_state.c`

| 字段 | 答案 | 状态 |
|---|---|---|
| 电池容量 | 140 mAh | 已确认 |
| 续航 | 8 小时+ | 已确认 |
| 续航对应使用场景 | 待产品确认（连续 EMG+IMU 采集 / 待机 / idle 三档需 QA 实测） | 待产品确认 |
| 充电方式 | 磁吸接口 (U3 + H1) | 已实现 |
| 充电电压（终止电压） | 4.15 V (`term-microvolt = 4150000`) | 已实现 |
| 充电电流 | 150 mA (`current-microamp = 150000`) | 已实现 |
| VBUS 输入限流 | 500 mA (`vbus-limit-microamp = 500000`) | 已实现 |
| 满充时间（理论） | ≈1 小时 (140 mAh / 150 mA, 不含 CV 阶段) | 估算，待实测 |
| 低电硬件提示 | ≤20% 红灯常亮；≤10% 红灯对称闪烁 (500/500 ms) | 已实现（`system_state.c::BAT_LOW_PCT / BAT_CRITICAL_PCT`） |
| 充电中能否采集 | 是（无硬件互斥，VBUS 接入仅影响 LED 优先级） | 已实现 |

> 充电状态 BLE 上报：BAS `0x180F` 推送电量百分比变化；充电中 / 已充满通过 `0xC103` Status 特征 byte[1] charge 字段上报。详见 [`ble-protocol.md §3.4 / §3.5`](./ble-protocol.md)。

---

## 4. 物理交互

### 4.1 按键（SW1，1 颗，PMIC SHPHLD 引脚）

物理：单按键，接 PMIC SHPHLD 引脚。

按键事件（短按 / 3 s 配对 / 6 s 关机 / 16 s 恢复出厂）和系统状态切换见 [`开关机与状态机逻辑.md §6`](./开关机与状态机逻辑.md)。

### 4.2 LED（1 颗 RGB，由 nPM1300 内置 LEDDRV 驱动）

驱动只支持 on/off，亮度由软件 PWM（62.5 Hz, 16 级）合成；色温修正 R:G:B = 8:2:4。

- 系统状态 → LED 颜色 / 节奏对应见 [`开关机与状态机逻辑.md §8`](./开关机与状态机逻辑.md)
- App 通过 BLE `0x04` 命令直接控制（覆盖系统模式）见 [`LED控制协议.md`](./LED控制协议.md)

### 4.3 振动器（DRV2605L + H2 马达）

ERM 开环 RTP 模式，强度 0–100，时长 1–65535 ms。

- 系统自动振动事件（开机 / 连接 / 关机 / 恢复出厂）见 [`开关机与状态机逻辑.md §9`](./开关机与状态机逻辑.md)
- App 通过 BLE `0xA0` 命令下发自定义振动见 [`马达控制协议.md`](./马达控制协议.md)

---

## 5. 设备状态机

> 来源：`src/system_state.c` + `src/main.c` + `src/ble_app.c` + `prj.conf` + `docs/开关机与状态机逻辑.md`

| 字段 | 答案 | 状态 |
|---|---|---|
| 开机方式 | SHPHLD 长按 ≥1 s（PMIC `ship-to-active-time-ms = 1008`）；插充电器**不自动开机** | 已实现 |
| 关机方式 | SHPHLD 长按 ≥6 s（latch + 松开后 commit；无 BLE 远程关机命令） | 已实现 / BLE 关机未实现 |
| 首次配对流程 | **Just Works**（无 passkey / OOB / 按键确认），自动 bonding | 已实现（`CONFIG_BT_SMP=y` + `CONFIG_BT_BONDABLE=y`） |
| 重连流程 | 已绑定设备 BLE 重连即可，无需重新配对；bond 存于 NVS（断电保留） | 已实现 |
| Bond 存储上限 | 最多 4 个手机，超出后覆盖最早一个（`CONFIG_BT_KEYS_OVERWRITE_OLDEST=y`） | 已实现 |
| idle 自动睡眠 | WAITING_CONNECT **3 min** 无连接 → STANDBY；STANDBY **5 min** → OFF | 已实现（`WAITING_TIMEOUT_MS / STANDBY_TIMEOUT_MS`） |
| STANDBY 唤醒 | 短按 SHPHLD → WAITING_CONNECT；BLE 不能唤醒 STANDBY（无活动广播以外的下行通路） | 已实现 |
| 多 App 并发连接 | **不允许**（`CONFIG_BT_MAX_CONN=1`），新连接被 controller 自动拒绝 | 已实现 |
| 失联后行为 | BLE 断开即调 `bt_le_adv_start` 重启广播；**不**保存采集到 Flash | 广播重启已实现 / Flash 缓冲未实现 |

---

## 6. 信号链

- 手环端做**滤波**和**编解码**
- 传到 App 的已经是"验证过的" EMG 数据（App 端不需要处理原始 ADC 噪声）
- App 端必须内置**对应的 decoder**，实现细节由固件团队维护，见 [`ble-protocol.md`](./ble-protocol.md)

---

## 7. 接口

详见 [`ble-protocol.md`](./ble-protocol.md)。

---

## 8. 主控芯片清单（附录，对 App 相关）

> **来源**：方案验证板 BOM（2026-05，开发中，可能调整）。下表只保留**对 App 端有意义**的核心 IC，完整电路 BOM 不在本仓库范围。
>
> **兼容性提醒**：若硬件团队后续替换 BLE 主控或 IMU，App 侧的库选择和数据格式都会受影响 —— 任何主控变更**先同步本文档**，再触发 App 改动。

| 位号 | 类别 | 型号 | 厂商 | 对 App 的影响 |
|---|---|---|---|---|
| U12 | **BLE 主控 MCU** | **nRF54L15-QFAA-R** | Nordic Semiconductor | 决定 Android BLE 库选 Nordic-BLE-Library；支持 BLE 5.4 全特性（2M PHY / Coded PHY / Extended Adv） |
| U4 | PMIC（电源管理） | nPM1300-CAAA-R7 | Nordic | 电量 / 充电状态可能走 Nordic 标准 service；Nordic 全家桶有利 |
| U17 | 8 通道 EMG AFE | ADS1298CZXGR | TI | 8ch × 2kHz EMG 数据源 |
| U5 | 6 轴 IMU | ICM-42670-P | TDK InvenSense | 加速度 × 3 + 陀螺 × 3，四元数由设备端融合后下发 |
| U13 | SPI Flash | W25Q16JVUXIQ (16Mbit = 2MB) | Winbond | 固件存储 / 可能的小量本地缓存；OTA 升级时 App 端需要 DFU |
| U2 | 触觉反馈电机驱动 | DRV2605LYZFR | TI | App 可下发振动指令；交互反馈（详见 §4.3） |
| H2 | 振动马达 | PZ200V-11-02P | XFCN | 同上 |
| LED1 | RGB LED | XL-1010RGB-YG | XINGLIGHT | 可视状态指示器（详见 §4.2） |
| SW1 | 物理按键 | TS24CA | SHOU HAN | 唯一物理按键（详见 §4.1） |
| L2 | 陶瓷天线 | RFECA3216060A1T | Walsin | App 端无影响，但室内 / 穿戴信号强度受其特性影响 |
| H1 | 电池接插件 | PZ200V-11-02P | XFCN | 容量 = 140 mAh（详见 §3） |
| X1 / X2 / X3 | 晶振（2.048MHz / 32.768kHz / 32MHz） | — | — | App 端无影响（影响硬件采样精度，由硬件保证） |

### 8.1 关键暗示（不要绕过）

- **BLE 主控 = Nordic nRF54L 系列** ⇒ Android 端**强烈推荐**使用 Nordic 官方库（详见 [`../android/plan.md`](../android/plan.md) §3.7）
- **PMIC 也是 Nordic** ⇒ 整个 BLE 端到端可以走 Nordic 标准 service（如 Battery Service, Device Information Service），不需要全部自定义
- **OTA 路径**：已实现 MCUboot + MCUmgr SMP-over-BLE，使用 nRF Connect Device Manager App 升级。详见 [`ble-protocol.md §4.4`](./ble-protocol.md)。
