# 操作运维 Runbook

> 这份文档面向"系统已经调通、要日常使用/排障"的场景。
> 详细调试过程见 `EXPERIMENT_LOG.md` 和 `DEBUG_RUNBOOK.md`。

---

## 一、启动顺序（每次开机）

### 主机 Orange Pi
开机即就绪。`sensor-host.service` 是 systemd 自启的，监听 `0.0.0.0:3333`(EMG) 和 `0.0.0.0:3334`(IMU)。

验证：
```bash
ssh orangepi@172.16.212.170 systemctl is-active sensor-host
# 应输出 active
```

### 从机 ESP32-S3
上电即开始执行下面的固定顺序（main.c）：
```
1. log_memory_snapshot("boot")          ← 1.5s 延时（让串口监控对上字节流）
2. nvs_flash_init                       ← NVS 准备
3. led_ctrl_init / npm1300_led_init     ← I2C1 + LED + 必须先于下一步
4. npm1300_enable_sensor_rails ★        ← 必须！ BUCK1 + LDO1 + LDO2 = 3.3V
5. wifi_manager_init                    ← 静态 IP 10.42.0.76 + WPA-WPA2-PSK + PMF off
6. wifi_manager_wait_connected
7. ads1298_init / start_conversion      ← chip ID 必须 0x92
8. ads1298_stream_start (TCP client)    ← connect 10.42.0.1:3333
9. lsm9ds1_init (best-effort) + IMU stream → connect 10.42.0.1:3334
10. heartbeat loop: 每 5 秒打 DRDY stats
```

**第 4 步是 root cause**：早期版本省略，sensor 全部无电。

---

## 二、典型健康日志（从机串口）

```
I (xxxx) npm1300_led: BUCK1 enabled @ 3300mV (IMU+FLASH rail)
I (xxxx) npm1300_led: LDO1 + LDO2 enabled @ 3300mV (ADS1298-A + ADS1298-B rails)
I (xxxx) npm1300_led: BUCK_STATUS = 0xC4 (BUCK1 ON, BUCK2 ON)
I (xxxx) npm1300_led: LDSW_STATUS = 0x1A (LDO1 ON, LDO2 ON)
I (xxxx) wifi_manager: static IP configured: 10.42.0.76/255.255.255.0 gw 10.42.0.1
I (xxxx) wifi:connected with OrangePi_AP, aid = 1, channel 11
I (xxxx) wifi_manager: WiFi connected, IP: 10.42.0.76
I (xxxx) ads1298: chip ID = 0x92
I (xxxx) ads1298: start_conversion done; DRDY isr_count after 50 ms = ~130, GPIO14 level = 0
I (xxxx) ads1298_stream: connected to 10.42.0.1:3333, streaming ADS1298 @ 2000 SPS
I (xxxx) main: DRDY: isr=N (+~10000 in 5s) take=K overflow=Z
```

主机端 `journalctl -u sensor-host` 期望：
```
[EMG] INFO slave connected from 10.42.0.76:xxxxx
[EMG] INFO EMG: ~1970 pkt/s, gaps=0
```

---

## 三、故障排查决策树

### 现象 A: 从机串口看不到 `npm1300_led: BUCK1 enabled`
→ I2C1 总线问题，检查 GPIO17/18 pull-up（R33/R34 = 10kΩ）。

### 现象 B: 有 BUCK1 但 `ads1298: chip ID = 0x00`
→ LDO1 没启用。检查 `LDSW_STATUS` 那行是不是 `LDO1 ON, LDO2 ON`。
→ 若 LDO1=OFF，看是否 `LDO1_MODE` write_verify 失败（PMIC I2C 不稳）。

### 现象 C: chip ID = 0x92 但 `DRDY isr_count = 0`
→ ADS1298 配置好了但不采样。检查 CONFIG3=0xCC（PD_REFBUF=1）有没有写入正确。
→ datasheet 要求 VCAP1 等 150ms；我们等 250ms 应该够。

### 现象 D: WiFi `reason=2 (AUTH_EXPIRE)` 反复
→ **AP 和 STA 不在同一信道**。这是 wlan0(AP) + wlP2p33s0(STA) 共用 WiFi 芯片，必须同信道。
→ 主机重启后 STA 可能连到 CYZX05 不同 BSSID（信道不同），AP 跟着切。
→ 修复（已固化在 NM 配置）：
```bash
sudo nmcli connection modify CYZX05 802-11-wireless.bssid 06:31:27:B0:7F:69
sudo nmcli connection down CYZX05 && sudo nmcli connection up CYZX05
sudo nmcli connection up orangepi-ap
```

### 现象 E: WiFi 连上了但 TCP `errno=118 (EHOSTUNREACH)`
→ 从机有 IP 但路由不通。等 1~2 秒（ARP 解析）通常自动恢复。

### 现象 F: TCP 连上但 `EMG: 50 pkt/s` 且每秒断
→ ADS1298 进入 quiet state（早期问题，PMIC 修复后已消失）。
→ 若再现，按从机 RESET 物理按键，让 chip 上电重启。

### 现象 G1: 16 通道都有数据但 chip-A 和 chip-B 信号 std 差 10 倍
→ Daisy chain 协议 OK（无丢包），但其中一片 ADS1298 的输入端悬空或电极接触不良。
→ 用 `scripts/verify_16ch.py /tmp/runN_emg.npz` 看各通道 std:
- 正常 EMG: std 数十万级
- 输入悬空: std 数百万级（接近 24-bit 满量程 ±8M）
→ 检查这片的：(a) 电极是否接好  (b) RLDIN/RLDREF 是否连接  (c) AVDD 是否稳定

### 现象 G2: 主机 `journalctl -u sensor-host` 无 `slave connected`
→ 检查 `arp -n | grep 10.42.0.76`：
- 显示 `(incomplete)` → 从机没真正在 AP 上关联
- 显示 MAC `70:04:1d:d8:1a:38` → 路由 OK，看从机 TCP 出口

---

## 四、有用的诊断命令

```bash
# 主机端
ssh orangepi@172.16.212.170
sudo iw dev wlan0 station dump            # 看从机有没有关联
arp -n | grep 10.42                        # ARP 表
ss -tn state established | grep ':3333'   # TCP 连接
tail -50 /var/log/sensor-host.log         # sensor-host 应用日志
journalctl -u sensor-host -n 100          # systemd 日志
sudo tcpdump -i wlan0 -nn 'port 3333'     # 抓包

# 离线分析
cd /home/orangepi/sensor_host
python3 align_streams.py --data-dir data --out-prefix /tmp/runN
cat /tmp/runN_report.txt
```

---

## 五、关键设计决策（避免反复踩坑）

| 决策 | 原因 |
|---|---|
| 静态 IP `10.42.0.76` | 新 ESP32 板 DHCP client 不稳，握手失败 |
| AP+STA 同信道 11 | 共用 WiFi 芯片硬件限制 #channels <= 1 |
| BUCK1+LDO1+LDO2 早期 enable | NPM1300 出厂默认 disabled，必须先供电再 SPI/I2C |
| LDSW mode reg per-channel (0x08/0x09) | Zephyr 源码确认；参考工程注释错误 |
| `read_serial.ps1` DTR=RTS=false | 默认 true 会通过 EN/IO0 自动 reset ESP32 |
| Phase 3 验收：3 × 65s, 0 gaps | 已证明 production-ready |

---

## 六、性能基线（Phase 3 实测）

- EMG 速率：1970 ± 3 pkt/s（≈ ADS1298 设计 2000 SPS）
- 包丢失：**0**（400k 包累计）
- 时钟漂移：< 7 ppm（基于线性回归 slope）
- 对齐精度：~11 μs（回归 std / √N）
- WiFi 链路：channel 11 (2462 MHz)，信号 -17 dBm（极强）
- 数据率：~1 Mbps（EMG only，IMU 未连）
