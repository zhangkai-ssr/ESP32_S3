# 双手环 AP 主机时间同步方案

本文档基于“分层同步”的思路设计本 ESP32-S3 双手环项目的时间同步方案：

- 能硬同步的部分尽量硬同步。
- 手环侧保留原始高频数据和 MCU 时间戳。
- AP/Host 侧记录接收时的主机时间。
- 后处理阶段建立 `MCU 时间戳 -> Host 时间轴` 的线性映射。

当前项目没有多路相机硬同步需求，因此这里把文档中的三个时钟域简化为两个核心时钟域：

- AP 主机时间：host / dongle / 上位机的公共时间轴。
- 手环 MCU 时间：每只 ESP32-S3 内部的 `esp_timer_get_time()` 微秒时间。

如果后续系统加入相机或外部动作捕捉，再把相机 PTS / PWM frame index 并入同一条 AP 主机时间轴。

## 目标

完整设备包含两个 ESP32-S3 手环节点：

- 左手环：`device_id = 1`
- 右手环：`device_id = 2`
- AP 主机：负责建网、收流、记录 host 时间、生成同步质量报告

同步目标不是让两个 ESP32 物理共享同一晶振，而是让两只手环采集到的 EMG / IMU 样本都能被换算到 AP 主机的统一时间轴上。

推荐首阶段目标：

- 200 Hz IMU 对齐误差进入毫秒级。
- 2 kHz EMG 保留原始 MCU 时间戳，后处理按 host 时间轴重建。
- 每个 episode 输出同步质量指标，包括丢包率、回归残差、有效样本率。

## 系统拓扑

```text
左手环 ESP32-S3  STA  \
                    Wi-Fi 2.4G      AP 主机 / Dongle / Host
右手环 ESP32-S3  STA  /             SoftAP + 时间同步 + 数据接收 + 后处理
```

建议 AP 主机固定参数：

- AP IP：`192.168.4.1`
- 时间同步 UDP 端口：`3332`
- EMG 数据端口：`3333`
- IMU 数据端口：`3334`

当前固件是 ESP32 侧开 TCP server、host 主动连接。双手环阶段建议逐步改为 ESP32 主动连接 AP 主机，因为 AP IP 固定，两个客户端主动汇聚到主机更适合最终 dongle / host 架构。

## 分层同步策略

### 第一层：在线轻量授时

在线阶段 AP 主机提供 UDP 时间同步服务，手环周期性发起同步请求，用于估计当前 `offset_us` 和链路 RTT。

四时间戳定义：

```text
T1 = 手环发送 sync request 时的 esp_timer 时间
T2 = AP 收到 request 时的 host monotonic 时间
T3 = AP 发送 response 时的 host monotonic 时间
T4 = 手环收到 response 时的 esp_timer 时间
```

手环计算：

```text
rtt_us    = (T4 - T1) - (T3 - T2)
offset_us = ((T2 - T1) + (T3 - T4)) / 2
```

手环侧同步时间：

```text
synced_time_us = esp_timer_get_time() + offset_us
```

工程建议：

- 启动后连续同步 20 次。
- 丢弃 RTT 最大的样本。
- 使用 RTT 最小样本或 offset 中位数作为初始 offset。
- 运行中每 1s 到 5s 重新同步一次。
- offset 缓慢滤波更新，不要让数据时间戳突然跳变。
- 数据包中仍保留原始 `mcu_ts_us`，不要只保留修正后的时间。

### 第二层：原始数据留痕

原文强调不要把传感器原始数据提前压到训练帧率。本项目同样适用：

- EMG 保留 2 kHz 原始包序列。
- IMU 保留 200 Hz 原始样本序列。
- 每个样本或批次必须携带 MCU 时间戳。
- AP 主机收到每个包时记录 host 接收时间。

推荐 AP 侧原始日志格式为 JSONL：

```json
{"episode_id":"20260601_001","device_id":1,"stream":"emg","seq":1024,"mcu_ts_us":183245678,"host_rx_us":987654321000,"payload_ref":"...","valid":true}
{"episode_id":"20260601_001","device_id":2,"stream":"imu","seq":88,"mcu_ts_us":183250000,"host_rx_us":987654326100,"samples":[...],"valid":true}
```

注意：`host_rx_us` 不是精确采样时间，它只是建立 MCU/Host 映射的观测值。真正用于后处理的时间应优先来自回归后的 `calibrated_time_us`。

### 第三层：后处理线性回归

每只手环、每类数据流单独拟合：

```text
host_time_us = slope * mcu_ts_us + intercept
```

拟合输入：

- `mcu_ts_us`：ESP32 采样或打包时的 MCU 时间戳。
- `host_rx_us`：AP 主机收到该包时的单调时钟时间。

拟合输出：

- `slope`：反映 ESP32 MCU 时钟相对 host 时间的比例和漂移。
- `intercept`：反映 ESP32 MCU 时间域相对 host 时间域的偏移。
- `residual_std_us`：同步质量指标。

后处理时每个样本的校准时间：

```text
calibrated_time_us = slope * mcu_ts_us + intercept
```

质量判断建议：

- 样本数少于 10：不做回归，回退到在线 offset 或 host 接收时间。
- `mcu_ts_us` 全为 0：判定该数据流没有有效 MCU 时间戳。
- 回归残差标准差大于 50 ms：判定拟合失败。
- 出现 MCU 时间回绕或非单调跳变：分段拟合或回退。

在线 UDP 授时提供实时粗对齐；后处理线性回归用于消除 Wi-Fi 收包、任务调度和短时链路抖动。

## 协议建议

### 时间同步包

UDP 端口：`3332`

请求：

```text
magic       uint32  "TSYN"
version     uint8   0x01
type        uint8   0x01 request
device_id   uint8   1=left, 2=right
reserved    uint8
seq         uint16
t1_us       int64
```

响应：

```text
magic       uint32  "TSYN"
version     uint8   0x01
type        uint8   0x02 response
device_id   uint8
reserved    uint8
seq         uint16
t1_us       int64
t2_us       int64
t3_us       int64
```

### 数据包扩展

当前 EMG 包是 64 bytes，字段里已有 32-bit `timestamp_us`。为了双手环同步，建议下一版协议升级：

- 增加 `device_id`。
- 增加 64-bit `mcu_ts_us` 或 episode 内 64-bit tick。
- 保留 `seq`，用于丢包和乱序检测。
- 保留原始采样时间，不要只发送 AP 修正时间。

如果短期不想破坏现有 `ads1298_tcp_client.py`，可以先不改 64B 数据包，而是在 AP 接收端按连接来源绑定 `device_id`，并额外记录 host 接收时间。后续再升协议版本。

IMU 包已经包含首样本时间戳，建议同样升级为 64-bit MCU 时间，并在每个样本中保留相对时间或固定采样间隔。

## 本项目落地路径

### 阶段 1：AP 主机收双手环

- AP 主机开 SoftAP，固定 IP 为 `192.168.4.1`。
- 两只 ESP32-S3 以 STA 模式连接 AP。
- AP 主机同时接收两个设备的数据流。
- 先用不同端口或配置文件区分左右手。

### 阶段 2：加入 UDP 时间同步

固件新增：

- `main/time_sync.h`
- `main/time_sync.c`

提供接口：

```c
void time_sync_start(void);
int64_t time_sync_now_us(void);
int64_t time_sync_offset_us(void);
bool time_sync_is_locked(void);
```

主流程建议：

```text
nvs_flash_init
wifi_manager_init
wifi_manager_wait_connected
time_sync_start
等待 locked 或达到最小同步次数
ads1298_stream_start
imu_stream_start
```

### 阶段 3：数据包携带 MCU 时间戳

修改点：

- `main/ads1298_stream.c`：包内时间戳从 32-bit 本地时间升级或旁路记录为 64-bit MCU 时间。
- `main/imu_stream.c`：IMU 样本保留 MCU 时间戳，并在 AP 侧记录 host 接收时间。
- `ads1298_tcp_client.py` / 新 AP 主机脚本：同步解析新字段。

### 阶段 4：后处理拟合

AP 主机记录原始 JSONL 后，新增脚本：

- 读取左右手 EMG / IMU 原始日志。
- 对每个 `device_id + stream` 拟合 `host_time_us = slope * mcu_ts_us + intercept`。
- 输出同步质量报告。
- 生成面向算法的数据集时，再按目标时间网格做最近邻、线性插值或窗口统计。

## 与当前代码的关系

当前代码中相关位置：

- `main/wifi_manager.c`：当前是 STA 模式连接外部 Wi-Fi，后续可继续作为手环客户端模式使用。
- `main/ads1298_stream.c`：当前 EMG TCP server 在 `3333` 端口监听，后续建议改为主动连接 AP 主机。
- `main/imu_stream.c`：当前 IMU TCP server 在 `3334` 端口监听，后续建议同样改为主动连接 AP 主机。
- `ads1298_tcp_client.py`：当前是单设备 host 验证工具，后续可演进为 AP 侧多设备收流器。

## 回退机制

同步系统不能假设每个 episode 都完美：

- UDP 同步失败：继续记录原始 `mcu_ts_us`，后处理使用 host 接收时间回退。
- 回归失败：回退到在线 offset 或 host 接收时间。
- 某只手环断流：训练数据中显式标记该设备数据无效。
- 某段 MCU 时间戳异常：分段拟合，不能默默填入看起来合理的值。
- AP wall-clock 不稳定：优先使用 monotonic clock 做 episode 内对齐；跨文件对齐再保存 realtime 起点。

## 验证指标

每次联调至少记录：

- 左右手 Wi-Fi RSSI。
- 每只手环 UDP 同步 RTT 均值、最小值、最大值。
- 每只手环 offset 漂移曲线。
- EMG / IMU 丢包率和序号跳变次数。
- MCU/Host 回归残差标准差。
- 每类传感器有效样本率。

物理验证建议：

- 用同一个敲击动作同时刺激左右手环，检查 EMG/IMU 波形峰值时间差。
- 如果后续加入相机，用 LED 闪烁或机械敲击同时进入图像和手环传感器，量化端到端误差。

## 推荐结论

本项目当前阶段建议采用：

```text
AP 主机建网和收流
两只 ESP32-S3 手环保留原始 MCU 时间戳
UDP 四时间戳在线估计 offset
AP 侧记录 host_rx_us
后处理做 MCU/Host 线性回归
最后按统一 host 时间轴生成算法数据
```

这比“开始采集时记一个 t0，然后按固定频率推算”更可靠，也符合当前项目“先验证链路、保留原始高频数据、再逐步增强同步精度”的开发阶段。
