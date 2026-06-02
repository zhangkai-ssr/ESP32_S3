# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## 项目身份

这是一个基于 **ESP-IDF 5.3.1**、目标芯片为 **ESP32-S3-PICO-N8R8** 的 ESP32-S3 固件项目，当前目标是为**双手佩戴式神经腕带**提供基础数据采集与传输能力。

当前系统定位是：
- 双手设备形态
- 通过 **2.4G / Wi‑Fi 链路** 将数据发送到 host / dongle 侧计算单元
- 当前使用 **1 × ESP32-S3-PICO-N8R8** 作为单板开发与链路验证平台
- 完整设备假设为 **2 个设备侧节点**，对应双手数据源

当前阶段仍是**数据链路验证优先**：重点不是本地推理或完整产品功能，而是先把 EMG 数据打包、持续推流、上位机接收与吞吐验证这条链路跑通。

## 常用命令

本项目是标准 **ESP-IDF** 应用。

```bash
# 标准构建
idf.py build

# 清理构建产物
idf.py fullclean

# 重新配置工程
idf.py menuconfig

# 烧录到设备
idf.py -p <PORT> flash

# 打开串口监视器
idf.py -p <PORT> monitor

# 一步完成烧录并打开监视器
idf.py -p <PORT> flash monitor

# 直接从生成的 build 目录构建 app 目标
cmake --build build --target app
```

当前与构建强相关的事实：
- 目标芯片是 `esp32s3`。
- 根目录 `CMakeLists.txt` 声明 ESP-IDF 工程。
- `main/CMakeLists.txt` 只注册了一个组件。
- 当前核心逻辑集中在 `main/main.c`，没有拆分为多个组件。

## 测试与验证

当前仓库没有现成的单元测试或集成测试；验证方式以**固件构建成功** + **真机联网推流** + **上位机 TCP 客户端收流**为主。

```bash
# 构建检查
idf.py build

# 烧录并查看启动日志
idf.py -p <PORT> flash monitor

# 在主机侧验证 TCP 数据流
python ads1298_tcp_client.py <ESP32_IP>

# 指定端口验证
python ads1298_tcp_client.py <ESP32_IP> 3333
```

当前验证重点：
- Wi‑Fi 是否成功联网并获取 IP
- TCP server 是否在 `3333` 端口正常监听
- 主机端是否可以持续收到固定长度包
- 包头、版本、序号、时间戳、设备数、包尾是否符合约定
- 长时间运行下是否存在断流、乱序、吞吐下降或明显抖动

## 设备数据约束

当前目标设备的关键假设如下：

- 双手佩戴式神经腕带
- 数据通过 **2.4G** 链路传输到 dongle / host 侧计算单元
- EMG 采样率：`2000 Hz`
- EMG 通道数：`16`
- EMG 精度：`24-bit`
- IMU：`9-axis`
- IMU 采样 / 更新频率：`200 Hz`（已实现）
- 指尖关节位置误差目标：`3 mm`
- 平均关节旋转误差目标：`0.68°`

注意：当前代码里还没有真实 ADS1298 / IMU 采集驱动，EMG 数据仍是**按协议格式构造的模拟数据流**，用于先验证网络传输与主机解码链路。

## 高层架构

当前仓库是一个非常小的单组件 ESP-IDF 工程：

- `CMakeLists.txt`：工程入口
- `main/CMakeLists.txt`：注册 `main` 组件
- `main/main.c`：完整固件主流程
- `ads1298_tcp_client.py`：主机侧 TCP 收流与校验工具

当前还没有把 Wi‑Fi、传输协议、采样驱动、状态管理拆成独立模块；如果要扩展功能，先确认是否真的需要拆分，避免在当前验证阶段过早抽象。

## 启动与运行主线

`main/main.c` 当前实现的是一个简化但完整的“联网后持续推流”流程：

1. 初始化 NVS。
2. 初始化 `esp_netif`、事件循环和 Wi‑Fi station。
3. 注册 Wi‑Fi / IP 事件处理器。
4. 使用硬编码 SSID / password 发起联网。
5. 等待拿到 station IP。
6. 创建 TCP server task，在 `3333` 端口监听。
7. 接受单个 client 连接。
8. 周期性构造一批固定格式 ADS1298 风格数据包并通过 TCP 发送。
9. 若 client 断开，则关闭 socket，回到等待下一次连接。

如果任务涉及启动流程、联网、socket 生命周期或推流节奏，优先阅读：
- `main/main.c`

## 数据链路与包格式

当前固件并不是直接读取真实 ADS1298，而是构造了一个固定协议格式的数据流，供 host 侧联调。

### 当前包结构

每个包大小固定为 `64 bytes`，字段顺序如下：

- `header`：`0xAA`
- `version`：`0x02`
- `sequence`：16-bit
- `timestamp_us`：32-bit 微秒时间戳
- `device_count`：`2`
- `payload`：2 个 ADS1298 数据块
- `footer`：`0x55`

### 当前 payload 假设

- `device_count = 2`，对应双手或双节点假设
- 每个 device block 为 `27 bytes`
- 每个 block 包含：
  - 1 byte 标识 (`0xC0`)
  - 1 byte 保留位
  - 1 byte device index
  - 8 路通道 × 3 bytes = 24 bytes
- 总计 `2 × 8 = 16` 路 24-bit EMG 通道

### 当前发送策略

- TCP 端口：`3333`
- 单批发送包数：`50`
- 单批字节数：`50 × 64 = 3200 bytes`
- 当前目标吞吐：`500000 B/s`
- 通过 `esp_timer_get_time()` + `vTaskDelay()` 控制发送节奏

如果协议结构变化，必须同时检查：
- `main/main.c`
- `ads1298_tcp_client.py`

不要只改单侧，否则 host 和 firmware 会立刻失配。

## 当前实现的关键事实

当前代码里有几个重要现实约束：

- Wi‑Fi 账号密码硬编码在 `main/main.c`。
- 目前没有 provisioning、配网页面、NVS 持久化配置、蓝牙配网等机制。
- 当前 TCP server 只服务**单个 client**；不是多连接广播架构。
- 当前发送的是**模拟 EMG 数据**，不是板上真实 AFE 采样结果。
- `app_main()` 在启动 server task 后基本进入空闲循环；系统当前是明显的 TCP-stream-first 结构。

如果后续任务涉及“为什么数据不是真实采样”“为什么不能多客户端同时连”“为什么 Wi‑Fi 不能改配置”，先看是不是当前实现本来就还没做，而不是默认当成 bug。

## Host 侧工具

当前仓库自带一个主机侧验证脚本：

- `ads1298_tcp_client.py`：用于连接设备 TCP server，检查包头包尾、长度、序号连续性与吞吐表现

任何涉及包格式、字段长度、版本号、设备数的修改，都应同步更新这个脚本。

## 需要优先理解的设计点

### 1. 当前目标是“先验证链路”，不是“完成最终硬件驱动”

很多参数已经在文档里指向最终产品规格，例如：
- 16 通道、24-bit、2 kHz EMG
- 9 轴 IMU
- 双手设备
- IMU 采样频率：200 Hz（已实现）

但这并不代表仓库当前已经把这些真实硬件全部接起来。现在的主线任务是把**协议、节奏、吞吐、host 侧解析**先稳定下来。

### 2. `main/main.c` 是单文件真相源

当前绝大多数行为都集中在一个文件中：
- Wi‑Fi 初始化
- 事件处理
- TCP server
- 包构造
- 批量发送

如果改启动、联网、协议、发送速率或序号逻辑，先从 `main/main.c` 全量阅读，不要假设仓库里还有其他抽象层替你兜底。

### 3. 协议参数和业务目标要区分开

像 `2000 Hz`、`16 channels`、`24-bit`、双手、IMU 9 轴，这些是**设备目标规格**；当前 TCP payload 则是**链路验证协议实现**。二者应尽量对齐，但在真实驱动尚未接入前，不要把“目标规格”误认为“当前所有字段都已经代表真实硬件状态”。

## 开发时的注意事项

- 如果修改包格式，务必同步更新 `ads1298_tcp_client.py`。
- 如果修改发送速率、batch 大小、时间戳或序号逻辑，注意 host 侧吞吐和连续性验证是否仍成立。
- 如果引入真实传感器采样，优先保证输出格式稳定，再考虑更复杂的调度或抽象。
- 如果要把 Wi‑Fi 配置从硬编码迁出，优先考虑最小可行方案，不要在当前阶段引入过重配置系统。
- 当前 README 原本是 ESP-IDF 默认模板；如果 README 与代码冲突，以代码和本文件为准。

## 后续合理扩展方向

如果后续继续演进，这个项目大概率会沿这些方向发展：

- 把模拟 ADS1298 数据替换为真实 AFE 采样链路
- 增加 IMU 采集与上送
- 引入更明确的数据帧版本管理
- 从单文件结构拆分出 Wi‑Fi、streaming、protocol、sensor 模块
- 将 Wi‑Fi 凭据改为可配置项
- 从 Wi‑Fi/TCP 验证链路演进到更贴近最终 2.4G / dongle 方案

做这些扩展时，优先保证**链路稳定性**与**host 工具同步演进**，不要为了结构美观打断当前验证主线。
