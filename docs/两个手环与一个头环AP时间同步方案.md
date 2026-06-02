# 两个手环与一个头环 AP 时间同步方案

本文档基于当前 ESP32-S3 神经腕带项目的文档、固件实现和主机侧脚本，说明“两只手环 + 一个头环 AP”形态下的数据时间同步方案。

当前阶段的核心目标不是做完整产品闭环，而是让两只手环的 EMG / IMU 数据能够稳定推流，并在头环 AP / host 侧换算到同一条可信时间轴上，便于后续算法处理、双手动作对齐和链路质量评估。

## 1. 系统角色

完整联调形态包含三个节点：

- 左手环：ESP32-S3 数据节点，建议 `device_id = 1`。
- 右手环：ESP32-S3 数据节点，建议 `device_id = 2`。
- 头环 AP：作为 2.4G / Wi-Fi AP、统一时间主机、数据接收端和后处理入口。

推荐拓扑如下：

```text
左手环 ESP32-S3  STA  \
                    2.4G / Wi-Fi       头环 AP / Host / Dongle
右手环 ESP32-S3  STA  /                SoftAP + 授时 + 收流 + 后处理
```

这里的“同步”不是让两只 ESP32-S3 物理共用同一个晶振，而是让两只手环产生的传感器样本都可以换算到头环 AP 的统一时间轴上。

## 2. 当前项目实现状态

当前仓库已经具备时间同步方案的部分实现：

- `main/time_sync.c` / `main/time_sync.h`：手环侧 UDP 四时间戳同步任务。
- `time_sync_server.py`：头环 AP / host 侧 UDP 时间同步服务，端口为 `3332`。
- `time_sync_postprocess.py`：基于 JSONL 日志做 `MCU 时间戳 -> host 时间轴` 线性回归。
- `main/main.c`：Wi-Fi 连接后启动 `time_sync_start()`，最多等待 5 秒锁定，再启动 EMG / IMU 数据流。
- `main/ads1298_stream.c`：EMG 数据包当前为 `0x03` 版本，包内包含 64-bit `esp_timer_get_time()` MCU 时间戳。
- `main/imu_stream.c`：IMU 数据包包含首样本 64-bit MCU 时间戳，采样目标为 200 Hz。

当前仍需注意的边界：

- 固件目前仍是 ESP32-S3 手环侧开启 TCP server，host 主动连接手环的 `3333` / `3334` 端口。
- 更适合最终三节点形态的方式，是两只手环作为 TCP client 主动连接头环 AP。
- 数据包内当前保留的是原始 MCU 时间戳，不建议直接替换为同步后的 host 时间。
- 头环 AP 侧还需要完善多设备同时收流、记录 `host_rx_us`、落 JSONL 和输出同步质量报告的工具链。

## 3. 固定端口与参数建议

建议头环 AP 使用固定 IP 和端口规划：

```text
头环 AP IP             192.168.4.1
UDP 时间同步端口       3332
EMG 数据端口           3333
IMU 数据端口           3334
```

手环侧配置项来自 `main/Kconfig.projbuild`：

- `CONFIG_DEVICE_ID`：区分左手环 / 右手环。
- `CONFIG_TIME_SYNC_SERVER_IP`：默认 `192.168.4.1`。
- `CONFIG_TIME_SYNC_PERIOD_MS`：默认 `2000 ms`，用于运行中周期性重新同步。

Wi-Fi 侧当前是 STA 模式连接外部 AP，`esp_wifi_set_ps(WIFI_PS_NONE)` 已在 `esp_wifi_start()` 后调用，适合降低 Wi-Fi 省电带来的链路抖动。

## 4. 两个时钟域

本方案只保留两个核心时钟域：

- 头环 AP 时间：host / dongle / 上位机的统一单调时间轴。
- 手环 MCU 时间：每只 ESP32-S3 内部 `esp_timer_get_time()` 微秒时间轴。

两只手环的 MCU 时钟彼此独立，会有启动偏移和晶振漂移；头环 AP 的任务是给它们提供统一参考，并在后处理中把两条 MCU 时间轴映射到同一个 AP 时间轴。

## 5. 在线 UDP 四时间戳同步

手环启动并连上 Wi-Fi 后，向头环 AP 的 UDP `3332` 端口发起同步请求。协议采用 NTP-lite 的四时间戳计算方式：

```text
T1 = 手环发送 request 时的 MCU 时间
T2 = 头环 AP 收到 request 时的 host monotonic 时间
T3 = 头环 AP 发送 response 时的 host monotonic 时间
T4 = 手环收到 response 时的 MCU 时间
```

手环侧计算：

```text
rtt_us    = (T4 - T1) - (T3 - T2)
offset_us = ((T2 - T1) + (T3 - T4)) / 2
```

同步后的估算时间：

```text
synced_time_us = esp_timer_get_time() + offset_us
```

当前固件实现策略：

- 启动时连续同步 20 轮。
- 选择 RTT 最小的一轮作为初始 offset。
- 运行中按 `CONFIG_TIME_SYNC_PERIOD_MS` 周期重新同步。
- 运行中 offset 使用低通滤波更新，避免时间戳突然跳变。
- 若 5 秒内未锁定同步，系统继续启动 EMG / IMU 流，但日志会标记使用原始 MCU 时间。

## 6. 为什么数据包仍保留 MCU 时间戳

数据包不建议只写入 `synced_time_us`，原因是在线同步会受到 Wi-Fi 抖动、任务调度和 UDP 丢包影响。如果只保留修正后的时间，一旦 offset 估计异常，后处理很难恢复。

推荐每个数据包至少保留：

- `device_id`
- `stream`
- `seq`
- 原始 `mcu_ts_us`
- AP 接收时记录的 `host_rx_us`
- 有效性标记 `valid`

头环 AP 侧原始日志建议使用 JSONL：

```json
{"episode_id":"20260602_001","device_id":1,"stream":"emg","seq":1024,"mcu_ts_us":183245678,"host_rx_us":987654321000,"valid":true}
{"episode_id":"20260602_001","device_id":2,"stream":"imu","seq":88,"mcu_ts_us":183250000,"host_rx_us":987654326100,"valid":true}
```

其中 `host_rx_us` 不是精确采样时间，而是用于建立 MCU / host 映射的观测值。真正用于算法对齐的时间应优先使用后处理得到的 `calibrated_time_us`。

## 7. EMG / IMU 数据流要求

EMG 当前目标：

- 16 通道。
- 24-bit。
- 2000 Hz。
- 当前 `main/ads1298_stream.c` 已使用 64-bit MCU 时间戳。
- 数据端口为 TCP `3333`。

IMU 当前目标：

- 9-axis。
- 200 Hz。
- 当前 `main/imu_stream.c` 已记录每批首样本 64-bit MCU 时间戳。
- 数据端口为 TCP `3334`。

两类数据流都应保留原生频率，不要在手环侧提前压缩成算法帧率。后续生成算法数据时，再按统一 AP 时间轴做最近邻、线性插值或窗口统计。

## 8. 头环 AP 侧后处理

头环 AP 收到左右手环数据后，对每个 `device_id + stream` 单独拟合：

```text
host_time_us = slope * mcu_ts_us + intercept
```

拟合输入：

- `mcu_ts_us`：手环侧采样或打包时的 MCU 时间戳。
- `host_rx_us`：头环 AP 收到该包时的单调时钟时间。

拟合输出：

- `slope`：反映 MCU 时钟相对 AP 时间轴的比例和漂移。
- `intercept`：反映 MCU 时间域相对 AP 时间域的偏移。
- `residual_std_us`：同步质量指标。

每个样本的校准时间：

```text
calibrated_time_us = slope * mcu_ts_us + intercept
```

当前仓库的 `time_sync_postprocess.py` 已提供这个回归和质量判断框架。

## 9. 推荐启动顺序

当前固件启动顺序建议保持为：

```text
nvs_flash_init
pmic_npm1300_init
wifi_manager_init
wifi_manager_wait_connected
time_sync_start
等待 time_sync_is_locked 或超时
ads1298_init / ads1298_start_conversion
ads1298_stream_start
lsm9ds1_init
imu_stream_start
```

头环 AP / host 侧建议启动顺序：

```text
开启 SoftAP
启动 time_sync_server.py
启动 EMG / IMU 多设备收流程序
等待左手环和右手环连接
开始 episode 记录
保存原始 JSONL
运行 time_sync_postprocess.py
输出同步质量报告和校准后数据
```

## 10. 从当前实现到三节点形态的落地步骤

### 阶段 1：单手环同步验证

- 头环 AP / host 启动 `time_sync_server.py`。
- 单个 ESP32-S3 连上 AP。
- 固件日志确认 `time sync locked`。
- 连接 EMG `3333` 和 IMU `3334`，确认时间戳单调递增、序号连续、吞吐稳定。

### 阶段 2：双手环接入

- 左右手环分别设置 `CONFIG_DEVICE_ID = 1 / 2`。
- 两只手环连接同一个头环 AP。
- AP 侧分别记录左右手环的 UDP 同步 RTT、offset、EMG / IMU 丢包率。
- 短期可以按 IP、端口或连接来源绑定左右手环。

### 阶段 3：头环 AP 多设备收流

- 将当前单设备 `ads1298_tcp_client.py` / `imu_tcp_client.py` 演进为多设备收流器。
- 每个包落盘时记录 `host_rx_us = time.perf_counter_ns() // 1000`。
- 日志统一写成 JSONL，方便 `time_sync_postprocess.py` 处理。

### 阶段 4：手环主动连接 AP

- 将 EMG / IMU 从“手环开 TCP server”逐步改为“手环主动连接头环 AP”。
- 头环 AP 作为固定服务端，更符合最终 dongle / host 汇聚架构。
- 连接建立后由 `device_id` 区分左手环和右手环。

### 阶段 5：同步质量报告

每个 episode 至少输出：

- 左 / 右手环 Wi-Fi RSSI。
- UDP 同步 RTT 最小值、均值、最大值。
- offset 漂移曲线。
- EMG / IMU 包率、丢包率、序号跳变次数。
- MCU / host 回归残差标准差。
- 有效样本率。

## 11. 回退机制

同步系统必须允许失败并留痕：

- UDP 同步失败：继续记录原始 `mcu_ts_us`，后处理回退到 `host_rx_us`。
- 初始锁定失败：固件继续推流，但日志标记 `time sync not locked`。
- 回归样本少于 10：不做线性回归。
- `mcu_ts_us` 全为 0 或非单调：判定该流时间戳异常，分段拟合或回退。
- 回归残差标准差大于 50 ms：判定拟合不可靠。
- 某只手环断流：训练数据中显式标记该侧无效，不做静默填补。

## 12. 物理验证建议

为了确认同步不是只在日志层面成立，建议做物理事件验证：

- 用同一个敲击动作同时刺激左右手环，检查 EMG / IMU 峰值在校准时间轴上的差值。
- 让头环 AP 记录左右手环同一 episode 的同步报告，比较两侧 RTT 和回归残差。
- 长时间运行 10 分钟以上，检查 offset 漂移、丢包率和序号连续性。
- 如果后续加入头环 IMU 或摄像头，可让同一动作同时进入头环传感器和手环传感器，用于端到端验证。

## 13. 推荐结论

当前项目最合适的时间同步策略是：

```text
头环 AP 建网、授时和收流
两只 ESP32-S3 手环保留原始 MCU 时间戳
UDP 四时间戳在线估计 offset
AP 侧记录 host_rx_us
后处理做 MCU / host 线性回归
最终按统一 AP 时间轴生成算法数据
```

这比“开始采集时记一个 t0，然后按固定采样率推算”更可靠，也更符合当前项目的数据链路验证阶段：先保证 EMG / IMU 原始数据、序号和时间戳完整，再逐步把双手环和头环 AP 的同步精度推进到毫秒级。
