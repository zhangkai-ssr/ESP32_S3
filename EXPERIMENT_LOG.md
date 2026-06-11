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

**结果 (E1)**: WiFi 关联成功但 DHCP 失败（4-way handshake OK，IP 拿不到），
ADS1298 init 阶段被 `wifi_manager_wait_connected` 阻塞，无法验证。

主机端确认：
- `iw dev wlan0 station dump` 能看到 STA `70:04:1d:d8:1a:38` 已关联
- `dnsmasq` 收不到任何 DHCP 包
- arp 表 `10.42.0.76 (incomplete)` 反向 ARP 失败

→ 触发 E1b 补丁解 WiFi。

---

## E1b - 静态 IP 替代 DHCP，unblock E1

**目的**: 跳过 DHCP，让 ESP32 直接用 10.42.0.76 静态 IP。这样 `wifi_manager_wait_connected`
能在关联后立即返回（因为 IP 提前已经设了），main 流程进入 ADS1298 init，
然后我们才能看到 DRDY 诊断行。

**Patch**: wifi_manager.c 加 `STATIC_IP_ENABLED 1`，在 `esp_netif_create_default_wifi_sta`
后调用 `esp_netif_dhcpc_stop` + `esp_netif_set_ip_info`。

**预期日志**:
- 启动后看到 `static IP configured: 10.42.0.76/255.255.255.0 gw 10.42.0.1`
- WiFi 关联成功后立即看到 `ADS1298 init start`（不再卡在 DHCP）
- 主机端 `arp -n` 应该能解析到 `10.42.0.76`

**结果**: 待用户烧录

---
