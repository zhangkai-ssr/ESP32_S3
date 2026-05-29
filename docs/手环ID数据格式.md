# 手环 ID 数据格式（实时更新）

**来源**: 当前固件 (`src/ble_service.h`, `src/ble_app.c`, `src/emg_acq.c`, `src/mulaw_encode.c`, `src/imu.c`)
**同步时间**: 2026-05-18
**状态**: 开发中，协议会演进

> 本文只描述上行数据包（EMG / IMU）的二进制格式。完整 BLE 协议（连接参数、下行命令、电池、状态、OTA 等）见 [`ble-protocol.md`](./ble-protocol.md)。

---

## 1. 总览

**角色**：手环 = BLE Peripheral，App = BLE Central。  
**通信方向**：上行（设备 → App）走 GATT Notify。

### 1.1 Service / Characteristic

| 名称 | 短 UUID | 完整 128-bit UUID | 用途 |
|---|---|---|---|
| Notify Service | 0xF100 | `0000F0DE-BC9A-7856-3412-7856341200F1` | 上行数据总服务 |
| EMG Char       | 0xC101 | `0000F0DE-BC9A-7856-3412-7856341201C1` | EMG 数据 Notify |
| IMU Char       | 0xC102 | `0000F0DE-BC9A-7856-3412-7856341202C1` | IMU 数据 Notify |
| RW Char        | 0xC201 | `0000F0DE-BC9A-7856-3412-7856341201C2` | 下行命令 / 查询回包 |

> 设备名常见为 `nRF54L15_2` / `nRF54L15_T`。连接后 App 订阅 `0xC101` / `0xC102` 的 CCC 即可开始接收数据。连接成功后 ~300 ms 内设备会暂停 Notify（让 MTU 协商 / 配对完成），属正常。

### 1.2 共同约定

- **msg_num（包序号）**：每个上行包第 2 字节（offset = 1）。1 字节，0~255 环绕递增。
  - EMG 与 IMU **各自独立递增**，互不占用。
  - 同一路流内 msg_num **严格连续**（步长恒为 1，环绕除外），App 可逐包判定丢包：

    ```c
    gap = (uint8_t)(current_seq - last_seq) - 1;
    if (gap > 0) lost_count += gap;
    last_seq = current_seq;
    ```

- **帧头 / 帧尾**：固定字节，用于包完整性校验，校验失败的包丢弃。
- **字节序**：
  - EMG legacy：12-bit MSB-first 紧凑流
  - EMG μ-law：单字节 int8，无端序问题
  - IMU：全部 int16 **小端**

### 1.3 EMG 格式选择

EMG 现在支持两种格式，由 App 通过 RW 命令切换：

| fmt | 名称 | 说明 |
|---|---|---|
| `0x00` | legacy | 旧版 12-bit packed，20 sample/通道/包，~100 pkt/s |
| `0x01` | μ-law | 新版 8-bit μ-law，30 sample/通道/包，~67 pkt/s |

**默认行为**：
- 上电默认 `fmt = 0x00`
- BLE 断开重连后自动恢复 `fmt = 0x00`
- App 主动发 `B0 05 01` 后，从**下一个 EMG 包**开始切到 μ-law

**切换 / 查询命令**：

| 命令 | 字节 | 说明 |
|---|---|---|
| SET_FMT | `B0 05 <fmt>` | 设置当前 EMG 格式 |
| QUERY_FMT | `B0 06` | 查询当前格式，设备回 `B0 06 <fmt>` |

### 1.4 速查总表

| | EMG legacy | EMG μ-law | IMU-D1 | IMU-D2 | IMU-D3 |
|---|---|---|---|---|---|
| Char UUID | 0xC101 | 0xC101 | 0xC102 | 0xC102 | 0xC102 |
| 帧头 / 帧尾 | 0xAA / 0x55 | 0xAA / 0x55 | 0xD1 / 0x1D | 0xD2 / 0x2D | 0xD3 / 0x3D |
| 包大小 | 243 B | 243 B | 35 B | 51 B | 83 B |
| 通道 / 数据点 | 8 通道 | 8 通道 | 4 个四元数 | 4 个 6 轴 | 4 个 (四元数 + 6 轴) |
| 每包采样数 | 20 / 通道 | 30 / 通道 | 4 | 4 | 4 |
| 采样率 | ~1000 Hz | ~1000 Hz | 200 Hz | 200 Hz | 200 Hz |
| 发包频率 | ~50 Hz | ~33 Hz | 50 Hz | 50 Hz | 50 Hz |
| 数据编码 | 12-bit 二补码 packed | 8-bit 有符号 μ-law | int16 | int16 | int16 |
| 字节序 | 12-bit 大端 packed | 单字节 | 小端 | 小端 | 小端 |
| 估算带宽 | ~95 Kbps | ~64 Kbps | ~14 Kbps | ~20 Kbps | ~33 Kbps |

> 采用 μ-law 的直接动机是：iOS 常见 BLE 链路约束约为 **15 ms × 1 PDU/event ≈ 66 pkt/s**。旧版 2000 Hz 配置下 legacy 100 pkt/s 会持续快于链路排空能力，导致 ACL TX buffer 堵塞、`bt_gatt_notify` 超时、EMG 线程阻塞、DRDY `ovf` 暴涨。当前固件已降到 1000 Hz，legacy 约 50 pkt/s、μ-law 约 33 pkt/s，这个约束已显著放宽，但 μ-law 仍更省空口。

---

## 2. EMG 数据包（Char 0xC101）

## 2.1 legacy 格式（fmt = 0x00）

### 基本属性

| 属性 | 值 |
|---|---|
| 包长 | **243 B** |
| 通道数 | 8 |
| 每包采样数 | 20 / 通道 |
| 采样率 | ~1000 Hz |
| 包速率 | ~50 pkt/s |
| 编码 | 12-bit **有符号补码** |
| 字节序 | 大端 bitstream（MSB 优先） |
| 物理单位 | μV，scale = 1 |
| 数值范围 | −2048 ~ +2047 |

### 包结构

| 字段 | 偏移 | 长度 | 值 / 含义 |
|---|---|---|---|
| 帧头 | [0] | 1 B | `0xAA` |
| msg_num | [1] | 1 B | 包序号 |
| CH1 | [2..31] | 30 B | 20 样本 × 12-bit packed |
| CH2 | [32..61] | 30 B | 同上 |
| CH3 | [62..91] | 30 B | 同上 |
| CH4 | [92..121] | 30 B | 同上 |
| CH5 | [122..151] | 30 B | 同上 |
| CH6 | [152..181] | 30 B | 同上 |
| CH7 | [182..211] | 30 B | 同上 |
| CH8 | [212..241] | 30 B | 同上 |
| 帧尾 | [242] | 1 B | `0x55` |

### 12-bit Packing 方案

每 2 个 12-bit 样本占 3 字节：

```text
S0 = 12 bit, S1 = 12 bit
Byte[0] = S0[11:4]
Byte[1] = (S0[3:0] << 4) | S1[11:8]
Byte[2] = S1[7:0]
```

---

## 2.2 μ-law 格式（fmt = 0x01）

### 基本属性

| 属性 | 值 |
|---|---|
| 包长 | **243 B** |
| 通道数 | 8 |
| 每包采样数 | 30 / 通道 |
| 采样率 | ~1000 Hz |
| 包速率 | ~33 pkt/s |
| 编码 | 8-bit **有符号 μ-law** |
| 字节序 | 单字节，无端序问题 |
| 物理意义 | App 端需先解码回近似 12-bit |
| 编码值范围 | −127 ~ +127 |

### 包结构

| 字段 | 偏移 | 长度 | 值 / 含义 |
|---|---|---|---|
| 帧头 | [0] | 1 B | `0xAA` |
| msg_num | [1] | 1 B | 包序号 |
| CH1 | [2..31] | 30 B | 30 个 8-bit μ-law 样本 |
| CH2 | [32..61] | 30 B | 同上 |
| CH3 | [62..91] | 30 B | 同上 |
| CH4 | [92..121] | 30 B | 同上 |
| CH5 | [122..151] | 30 B | 同上 |
| CH6 | [152..181] | 30 B | 同上 |
| CH7 | [182..211] | 30 B | 同上 |
| CH8 | [212..241] | 30 B | 同上 |
| 帧尾 | [242] | 1 B | `0x55` |

### 通道内顺序

每通道 30 字节，逐样本顺序排列：

```text
ch_byte[0]  = sample[0] 的 μ-law 编码
ch_byte[1]  = sample[1] 的 μ-law 编码
...
ch_byte[29] = sample[29] 的 μ-law 编码
```

### μ-law 编码公式

把 12-bit signed 输入 `x ∈ [-2048, 2047]` 压缩成 8-bit signed 输出 `y ∈ [-127, 127]`：

```text
y = sign(x) × log(1 + μ × |x| / V) / log(1 + μ) × 127
```

参数：
- `μ = 255`
- `V = 2047`

然后：
- round to nearest
- clip 到 `[-127, +127]`

### 解码公式

```text
x = sign(y) × ((1 + μ)^(|y|/127) - 1) / μ × V
```

> 固件端只做编码；解码在 App / 上位机侧完成。

### 固件实现方式

固件不实时算 `log()`，而是查 **4096 项 LUT**：

```c
encoded = MU_LAW_ENCODE_LUT[x + 2048];
```

- 输入范围：`x ∈ [-2048, 2047]`
- LUT 大小：4096 B
- 超范围输入先 clamp

---

## 2.3 解包参考代码（Python）

### legacy 解包

```python
def unpack_12bit(buf: bytes):
    out = []
    for i in range(10):
        b0, b1, b2 = buf[i*3], buf[i*3+1], buf[i*3+2]
        s0 = (b0 << 4) | (b1 >> 4)
        s1 = ((b1 & 0x0F) << 8) | b2
        if s0 & 0x800: s0 -= 0x1000
        if s1 & 0x800: s1 -= 0x1000
        out.append(s0)
        out.append(s1)
    return out
```

### μ-law 解码

```python
import numpy as np

MU = 255.0
VMAX = 2047.0

def make_mulaw_decode_lut():
    y = np.arange(-128, 128, dtype=np.int32)
    sign = np.sign(y).astype(np.float64)
    yn = np.clip(np.abs(y) / 127.0, 0.0, 1.0)
    x = sign * ((1.0 + MU) ** yn - 1.0) / MU * VMAX
    return np.clip(np.round(x), -2047, 2047).astype(np.int16)

DECODE_LUT = make_mulaw_decode_lut()

def unpack_mulaw(buf: bytes):
    encoded = np.frombuffer(buf, dtype=np.int8)
    return DECODE_LUT[encoded.astype(np.int32) + 128]
```

### 按 fmt 统一解包

```python
def parse_emg(pkt: bytes, fmt: int):
    assert len(pkt) == 243
    assert pkt[0] == 0xAA and pkt[-1] == 0x55
    seq = pkt[1]
    channels = []
    for ch in range(8):
        off = 2 + ch * 30
        block = pkt[off:off + 30]
        if fmt == 0x01:
            channels.append(unpack_mulaw(block))   # 30 样本
        else:
            channels.append(unpack_12bit(block))   # 20 样本
    return seq, channels
```

---

## 3. IMU 数据包（Char 0xC102）

IMU 有 **D1 / D2 / D3** 三种帧模式，共享相同的特征 UUID，靠**首字节帧头**区分：
- `0xD1` → 仅四元数
- `0xD2` → 仅 6 轴原始数据
- `0xD3` → 四元数 + 6 轴

> **当前默认模式 = D3**。EMG 切到 μ-law **不会影响 IMU 格式**。

### 3.1 共同属性

| 属性 | 值 |
|---|---|
| 采样率 | 200 Hz |
| 每包采样数 | 4（累积 20 ms） |
| 包速率 | 50 pkt/s |
| 字节序 | **小端** (int16 LE) |
| 包内时间步进 | 5 ms / 样本 |

### 3.2 单个四元数布局（8 B，D1 / D3 用）

| 偏移 | 长度 | 字段 | 编码 |
|---|---|---|---|
| +0 | 2 B | qw | int16 LE，`(int16_t)(qw × 32767.0)` |
| +2 | 2 B | qx | 同上 |
| +4 | 2 B | qy | 同上 |
| +6 | 2 B | qz | 同上 |

### 3.3 单个 6 轴布局（12 B，D2 / D3 用）

| 偏移 | 长度 | 字段 | 编码 |
|---|---|---|---|
| +0 | 2 B | ax | int16 LE，±4 g 满量程 |
| +2 | 2 B | ay | 同上 |
| +4 | 2 B | az | 同上 |
| +6 | 2 B | gx | int16 LE，±500 dps 满量程 |
| +8 | 2 B | gy | 同上 |
| +10 | 2 B | gz | 同上 |

物理量换算：
- `accel_g  = raw / 32768 × 4`
- `gyro_dps = raw / 32768 × 500`

> 设备端已做 **轴交换 X↔Y**（修正横滚 / 仰俯，见 `src/imu.c`），App 端拿到的是已交换的数据，不需要再处理。

### 3.4 D1 包结构（仅四元数，35 B）

| 偏移 | 长度 | 内容 |
|---|---|---|
| [0]      | 1 B | 帧头 `0xD1` |
| [1]      | 1 B | msg_num |
| [2..9]   | 8 B | 四元数 #1（t + 0 ms） |
| [10..17] | 8 B | 四元数 #2（t + 5 ms） |
| [18..25] | 8 B | 四元数 #3（t + 10 ms） |
| [26..33] | 8 B | 四元数 #4（t + 15 ms） |
| [34]     | 1 B | 帧尾 `0x1D` |

### 3.5 D2 包结构（仅 6 轴，51 B）

| 偏移 | 长度 | 内容 |
|---|---|---|
| [0]      | 1 B  | 帧头 `0xD2` |
| [1]      | 1 B  | msg_num |
| [2..13]  | 12 B | 6 轴 #1（t + 0 ms） |
| [14..25] | 12 B | 6 轴 #2（t + 5 ms） |
| [26..37] | 12 B | 6 轴 #3（t + 10 ms） |
| [38..49] | 12 B | 6 轴 #4（t + 15 ms） |
| [50]     | 1 B  | 帧尾 `0x2D` |

### 3.6 D3 包结构（四元数 + 6 轴，83 B）

| 偏移 | 长度 | 内容 |
|---|---|---|
| [0]      | 1 B  | 帧头 `0xD3` |
| [1]      | 1 B  | msg_num |
| [2..21]  | 20 B | 数据点 #1（t + 0 ms）= 四元数 8B + 6 轴 12B |
| [22..41] | 20 B | 数据点 #2（t + 5 ms） |
| [42..61] | 20 B | 数据点 #3（t + 10 ms） |
| [62..81] | 20 B | 数据点 #4（t + 15 ms） |
| [82]     | 1 B  | 帧尾 `0x3D` |

每个 20 B 数据点内部：

| 偏移 | 长度 | 内容 |
|---|---|---|
| +0..+7   | 8 B  | 四元数（qw, qx, qy, qz，各 int16 LE） |
| +8..+19  | 12 B | 6 轴（ax, ay, az, gx, gy, gz，各 int16 LE） |

### 3.7 解包参考代码（Python，D3）

```python
import struct

IMU_D3_HEADER = 0xD3
IMU_D3_FOOTER = 0x3D
IMU_D3_PKT_SIZE = 83
ACC_FS_G = 4.0
GYRO_FS_DPS = 500.0

def parse_imu_d3(pkt: bytes):
    assert len(pkt) == IMU_D3_PKT_SIZE
    assert pkt[0] == IMU_D3_HEADER and pkt[-1] == IMU_D3_FOOTER
    seq = pkt[1]
    points = []
    for i in range(4):
        off = 2 + i * 20
        qw, qx, qy, qz = struct.unpack_from("<hhhh", pkt, off)
        ax, ay, az, gx, gy, gz = struct.unpack_from("<hhhhhh", pkt, off + 8)
        points.append({
            "quat":  (qw / 32767.0, qx / 32767.0, qy / 32767.0, qz / 32767.0),
            "accel_g":  (ax / 32768.0 * ACC_FS_G,
                         ay / 32768.0 * ACC_FS_G,
                         az / 32768.0 * ACC_FS_G),
            "gyro_dps": (gx / 32768.0 * GYRO_FS_DPS,
                         gy / 32768.0 * GYRO_FS_DPS,
                         gz / 32768.0 * GYRO_FS_DPS),
        })
    return seq, points
```

---

## 4. 时间轴与丢包

- 每路流的时间轴**独立**展开：
  - EMG legacy：包周期 ≈20 ms，包内样本步进 1 ms
  - EMG μ-law：包周期 ≈30 ms，包内样本步进 1 ms
  - IMU：包周期 20 ms，包内样本步进 5 ms
- App 端把首包到达时刻记作 `t0`，后续样本时间 = `t0 + 累计样本数 × 步进`。
- EMG / IMU 的 `msg_num` **独立递增**，本身不反映跨流时间关系，请各自维护时间轴。
- 设备端**不做应用层重传**；BLE 链路层有 PDU 重传保证基础可靠性。BLE 断开期间设备**不缓存**数据。

更详细的连接参数、丢包处理、电池上报、下行命令格式见 [`ble-protocol.md`](./ble-protocol.md)。
