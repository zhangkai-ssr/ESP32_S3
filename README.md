# ESP32-S3 神经腕带数据链路验证项目

这是一个基于 **ESP-IDF 5.3.1** 和 **ESP32-S3-PICO-N8R8** 的固件项目，用于验证双手神经腕带的数据采集与传输链路。

当前实现重点不是完整产品功能，而是先跑通：
- Wi‑Fi 联网
- TCP 持续推流
- 16 通道 24-bit EMG 风格数据打包
- host / dongle 侧接收、解析与吞吐验证

## 项目目标

面向的目标设备规格如下：

- 数据传输方式：**2.4G / host-dongle 侧计算链路**
- 佩戴形态：**双手佩戴**
- 主芯片：**ESP32-S3-PICO-N8R8**
- EMG 采样率：**2000 Hz**
- EMG 通道数：**16 通道**
- EMG 精度：**24 位**
- IMU：**9 轴**
- IMU 采样 / 更新频率：**200 Hz**（已实现）
- 指尖关节位置误差目标：**3 mm**
- 平均关节旋转误差目标：**0.68°**

注意：以上是系统目标规格；**当前仓库代码主要实现的是 EMG 风格模拟数据的网络推流验证**，并未完成真实传感器驱动接入。

## 当前实现内容

当前固件在 `main/main.c` 中实现了完整的基础流程：

1. 初始化 NVS
2. 初始化 Wi‑Fi station 模式
3. 连接预设 Wi‑Fi
4. 等待获取 IP 地址
5. 启动 TCP server
6. 接收一个客户端连接
7. 按固定节奏发送模拟 ADS1298 数据包

当前 host 侧工具：

- `ads1298_tcp_client.py`

它可用于：
- 连接设备 TCP server
- 校验包头 / 包尾
- 检查序号连续性
- 统计吞吐率
- 验证协议格式是否匹配

## 数据包格式

当前每个 TCP 数据包固定为 **64 字节**，格式如下：

- header: `0xAA`
- version: `0x02`
- sequence: 16-bit
- timestamp: 32-bit 微秒时间戳
- device count: `2`
- payload: 2 个 device block
- footer: `0x55`

每个 device block：
- 1 byte 标识
- 1 byte 保留
- 1 byte device index
- 8 通道 × 3 byte = 24 byte

因此当前协议总共承载：
- `2 × 8 = 16` 路通道
- 每通道 `24-bit`

## 目录结构

```text
├── CMakeLists.txt
├── README.md
├── CLAUDE.md
├── ads1298_tcp_client.py
└── main
    ├── CMakeLists.txt
    └── main.c
```

说明：
- `main/main.c` 是当前固件主逻辑真相源。
- `ads1298_tcp_client.py` 是 host 侧协议验证工具。
- `CLAUDE.md` 用于说明项目上下文、架构和开发注意事项。

## 构建与运行

### 1. 构建

```bash
idf.py build
```

### 2. 清理构建目录

```bash
idf.py fullclean
```

### 3. 烧录到设备

```bash
idf.py -p <PORT> flash
```

### 4. 打开串口监视器

```bash
idf.py -p <PORT> monitor
```

### 5. 一步烧录并监视

```bash
idf.py -p <PORT> flash monitor
```

### 6. 重新配置工程

```bash
idf.py menuconfig
```

## Host 侧验证

当设备联网成功并输出 IP 地址后，可在 PC 侧运行：

```bash
python ads1298_tcp_client.py <ESP32_IP>
```

如果要指定端口：

```bash
python ads1298_tcp_client.py <ESP32_IP> 3333
```

## 当前实现细节

### Wi‑Fi

当前 Wi‑Fi 凭据写死在 `main/main.c` 中：

- SSID: `S50`
- Password: `12345678qwe`

这意味着当前项目：
- 还没有配网页面
- 没有蓝牙配网
- 没有通过 NVS 保存用户配置
- 没有运行时修改网络参数的机制

### TCP 推流

当前 TCP server 默认监听：

- 端口：`3333`

当前发送策略：
- 每批发送 `50` 帧
- 每帧 `64` 字节
- 单批 `3200` 字节
- 目标发送速率约 `500000 B/s`

当前仅支持：
- **单客户端连接**
- 客户端断开后重新等待下一次连接

## 验证建议

当前没有单元测试；建议按以下方式验证：

1. 运行 `idf.py build`，确认构建成功。
2. 烧录到 ESP32-S3 设备。
3. 查看串口日志，确认 Wi‑Fi 成功拿到 IP。
4. 在 PC 上运行 `ads1298_tcp_client.py` 连接设备。
5. 检查：
   - 是否能持续收包
   - 包长是否恒定为 64 字节
   - 包头 / 包尾是否正确
   - sequence 是否连续
   - 吞吐是否稳定

## 开发注意事项

- 如果修改了包格式，必须同步修改 `ads1298_tcp_client.py`。
- 当前发送的是模拟数据，不要误认为已经接入真实 ADS1298。
- 如果要接入真实 EMG / IMU 采样，优先保持对 host 的协议兼容性。
- 当前逻辑主要集中在 `main/main.c`，修改前建议先通读完整文件。
- 当前 README 已更新为项目说明；如与代码冲突，以代码为准。

## 后续可能演进方向

这个项目后续可以继续向下列方向扩展：

- 接入真实 ADS1298 / AFE 数据采集
- 增加 IMU 实时采集与传输
- 拆分 Wi‑Fi、协议、streaming、sensor 模块
- 支持 Wi‑Fi 配置持久化
- 优化时间同步与数据帧版本管理
- 从 Wi‑Fi/TCP 验证逐步演进到最终 2.4G / dongle 方案

当前最重要的仍然是：**先把链路、协议和 host 验证工具稳定下来。**
