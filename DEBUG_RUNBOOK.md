# EMG 稳定性调试 Runbook

> 本文件是本轮调试的 SOP（标准操作流程）。任何一次实验都按这里的步骤走。

## 1. 问题陈述（截至 2026-06-11）

ESP32-S3 从机通过 WiFi (`OrangePi_AP` 2.4G ch11) 向 Orange Pi 5 Plus 主机
发送 EMG (ADS1298, 64B/包) 和 IMU (LSM9DS1TR, 211B/包) 数据流。
主机端 `sensor-host` 服务（位于 `/home/orangepi/sensor_host/`）listen 在
3333/3334，已工程化完成。

**故障现象**：
- IMU 流稳定，200 Hz 全程零丢包。
- EMG 流**首次 TCP connect 后 50ms 内 DRDY 中断停止**，chip 进入 "quiet
  state"（能响应 SPI register read，但不输出 DRDY，GPIO14 锁定 HIGH，
  `isr_count` 冻结）。
- 从机不停 TCP reconnect + ADS1298 stop/start 循环都无法恢复，必须冷
  启动。
- 已确认旧 ESP32 板的 ADS1298 物理损坏；新板（MAC `70:04:1d:d8:1a:38`）
  WiFi 兼容性已解（PMF capable=false）。

## 2. Root Cause 假设（按可能性排）

| # | 假设 | 证据 | 验证方式 |
|---|---|---|---|
| H1 | **SPI 任务跑在 CPU0，与 WiFi 抢中断**，导致 SPI burst 被 WiFi 抢占破坏，某个 byte 被 ADS1298 误解为 STOP/SDATAC 命令 | 社区文档 + 现象时序（connect 瞬间 DRDY 停） | 把 ADS1298 task + SPI bus 绑到 CPU1 |
| H2 | VCAP1 没等够 → CONFIG3 写入后 REFBUF 未稳定 → ADC 模拟模块没启动 | datasheet 要求 150ms，代码只等 2ms | 已 patch（加 250ms wait），需测试 |
| H3 | DRDY ISR 在 IRAM，但 ISR service 可能没被注册到 IRAM | 代码 IRAM_ATTR，但 ISR service install flag 是 0 | 改 install_isr_service 加 IRAM flag |
| H4 | DMA buffer 未 4 字节对齐 | `static uint8_t s_batch_buf[3200]` 默认 1 字节对齐 | 加 alignment attribute |
| H5 | TCP send 阻塞期间 WiFi RX 中断打断 SPI | 现象与 connect 时机吻合 | acquire SPI bus lock 或 critical section |

## 3. 工作目录与分支

- **基线分支**：`zhangkai1` （位于 `C:\work1\JSZN\ESP32_S3\ESP32-S3`）
  - WIP commit `5507043`: 加了 VCAP1 wait + WAKEUP + isr 诊断 + PMF 修复
- **调试分支**：`emg-stabilize` （位于 `C:\work1\JSZN\ESP32_S3\worktree-emg-stabilize`）
  - 后续所有 patch 提交到这个分支
- **回退**：如果某个 patch 把事情搞糟，直接 `git reset --hard HEAD~1`
- **合并**：调试完成后 `git checkout zhangkai1 && git merge emg-stabilize`

## 4. 单轮调试循环

```
[Claude] 在 emg-stabilize 分支 patch 代码 → commit
[Claude] 给用户编译命令: cd worktree-emg-stabilize && idf.py build && idf.py flash monitor
[用户]   烧录，等待 system ready
[用户]   把指定时段的串口日志贴回来
[Claude] 抓主机端日志: ssh orangepi@10.42.0.1 'journalctl -u sensor-host -n 100'
[Claude] 分析两侧日志 → 给出 verdict (passed / hypothesis rejected / new hypothesis)
[Claude] 决定下一轮（patch / commit / rollback）
```

## 5. 串口日志收集约定

用户每次贴日志时请包含：

1. **boot log**：从 `boot: ESP-IDF v5.3.1` 到 `System ready` 这一段
   - 我需要看 chip ID, register readback, **start_conversion done 那一行的 isr_count**
2. **30 秒运行日志**：System ready 之后 30 秒的所有 `I/W/E` 行
   - 我需要看 heartbeat 中的 `DRDY: isr=N (+M in 5s)` 行
   - 我需要看 `EMG TCP client` 的 connect/disconnect 事件
   - 我需要看 IMU 是否同时正常

## 6. 主机端自动化（我可以全自动）

| 操作 | 命令 |
|---|---|
| 启动/停止 sensor-host | `ssh orangepi@172.16.212.170 'sudo systemctl start sensor-host'` |
| 看实时日志 | `journalctl -u sensor-host -f` 或 `tail -f /var/log/sensor-host.log` |
| 看从机连接 | `ss -tn state established \| grep -E ':3333\|:3334'` |
| DHCP lease | `sudo cat /var/lib/NetworkManager/dnsmasq-wlan0.leases` |
| 抓 TCP 包 | `sudo tcpdump -i wlan0 -nn 'port 3333 or port 3334' -c 200` |
| 离线对齐分析 | `cd /home/orangepi/sensor_host && python3 align_streams.py --data-dir data --out-prefix /tmp/runN` |
| 清理数据 | `rm -f /home/orangepi/sensor_host/data/*.bin` |

## 7. 成功标准

- **必须达到**：连续 60 秒采集，EMG `+M in 5s` ≥ 10000，sequence gaps = 0
- **必须达到**：线性回归质量门 passed（残差 std < 50ms）
- **加分项**：多次 `systemctl restart sensor-host` 不需要冷启动从机
- **加分项**：把 IMU 流的 inter-arrival p99 控制在 60ms 以内

## 8. 当前测试状态

| 阶段 | 状态 |
|---|---|
| 旧 ESP32 板（MAC `80:65:99:...`） | ❌ ADS1298 chip 物理损坏 |
| 新 ESP32 板（MAC `70:04:1d:...`） | 🟡 WiFi 已通，传感器是否在线待测 |
| Phase 1 摸底 | 待执行（用户最新 boot log 的 DRDY 诊断行是关键） |
| Phase 2 修复 | 等 Phase 1 |
