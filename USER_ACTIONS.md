# 用户每轮要做的事（极简版）

每轮我会告诉你这次的实验编号（如 E1, E2, ...）。你只需要做这 3 步：

## 1. 从 worktree 编译烧录

```powershell
cd C:\work1\JSZN\ESP32_S3\worktree-emg-stabilize
idf.py build
idf.py flash monitor
```

> 首次进 worktree 编译会比平时慢（重新生成 build/）。后续就是增量。

## 2. 抓串口日志

启动后看到 `System ready  EMG TCP:3333  IMU TCP:3334` 后，**继续监控 30 秒**，
然后把从 `boot: ESP-IDF` 到第 6 条 heartbeat (`DRDY: isr=...`) 的日志整段贴给我。

> MobaXterm 选中右键复制即可。

## 3. 如果遇到我没料到的卡死，告诉我

主机这边的 sensor-host 服务、AP、DHCP 我会全自动控制，**你不需要碰主机端**。
