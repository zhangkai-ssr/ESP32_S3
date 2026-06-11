# 实验日志

每轮一个 section。Claude 维护。

---

## E1 - 验证新 ESP32 板上的传感器在线状态（baseline）

**目的**: 仅验证当前 WIP 代码（VCAP1 wait + WAKEUP + DRDY 诊断 + PMF 修复）
在新 ESP32 上是否能 boot 起来并报告 ADS1298 / LSM9DS1TR 是否在线。**不修改代码**。

**待验证的关键信号**:
1. `chip ID = 0x92` (ADS1298 SPI 通信)
2. `WHO_AM_I: AG=0x68 M=0x3D` (LSM9DS1TR I2C 通信)
3. `start_conversion done; DRDY isr_count after 50 ms = N` 这一行的 `N`
4. 五次 heartbeat 中 `DRDY: isr=N (+M in 5s)` 的 `+M`

**预期结果** (按硬件状态分类):
- 新板带传感器 + chip 健康 → `N > 100`，`+M > 10000`
- 新板带传感器 + 但 chip 异常 → `chip ID` 读对但 `N = 0`
- 新板没焊传感器 → chip ID 读不出 0x92，init 失败

**主机端动作**: sensor-host 暂停（已 stop），避免 TCP 干扰单纯测 DRDY。

**结果**: 待用户烧录后贴日志

---
