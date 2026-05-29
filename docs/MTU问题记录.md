# MTU 协商问题记录（Post-mortem）

> **类型**：单一主题问题复盘
> **时间线**：发现 → 误判 → 实测证伪 → 根因确认 → Phase A / B 落地（2026-05-12）
> **相关代码**：`src/ble_app.c`（MTU 兜底 + 门控）、`src/emg_acq.c` / `src/imu.c`（错误可见）、根 `Kconfig` + `prj.conf`
> **配套文档**：
> - [`architecture.md §5`](./architecture.md) — 设计权威
> - [`ble-protocol.md §2`](./ble-protocol.md) — 协议约定
> - [`上位机调试-MTU临时方案.md`](./上位机调试-MTU临时方案.md) — App 侧临时绕过方案（Phase A 落地后可逐步淡化）

---

## 1. 一句话结论

**HID + EMG 同 BLE 连接下 EMG 收不到包的根因 = ATT MTU 协商未完成**（停在默认 23，payload 上限 20 B < EMG 包 243 B），导致 `bt_gatt_notify` 返 `-EMSGSIZE` 被 `(void)` 吞掉、静默丢弃。**不是** TX 队列竞争、带宽不足、conn event 时隙抢占或 HID 优先级问题。

---

## 2. 症状

- 固件看起来正常：ADS1298 在采、滤波在跑、打包在跑、CPU 占用和电流都在 Stream 档位。
- App 订阅 `0xC101` / `0xC102` CCCD 返回成功。
- **手机端收到 0 包 EMG**，而 `0x2A19` 电池（1 B）、`0xC201` RW 响应都正常。
- IMU-D3（83 B）同样受影响，D1（35 B）/ D2（51 B）在某些 central 上能过（MTU 介于 23 和 86 之间时）。
- **平台差异明显**：
  - nRF Connect Android 默认 → 0 包
  - nRF Connect Android 手动 "Request MTU 247" → 立即出数据
  - PC Python `bleak` → 一直正常（bleak 默认会协商 MTU=247）
  - iOS CoreBluetooth → 自动协商，通常到 185 左右，IMU 能过、EMG 偶发失败

---

## 3. 走过的弯路（已否决假设）

| 假设 | 证伪依据 |
|---|---|
| "HID 报文抢占 BLE 时隙，EMG 被饿死" | 关掉 HID、只连 EMG，Android 默认 MTU 仍然收不到 |
| "ATT TX 队列深度不够 / 缓冲拥塞" | 增大 `CONFIG_BT_ATT_TX_COUNT` 无改善 |
| "带宽不够，需要 2M PHY" | 1M PHY 在 MTU=247 下也稳跑 ~227 kbps |
| "连接间隔太长，Notify 来不及发" | 调到 7.5 ms 无改善；MTU=247 下 15 ms 也能稳跑 |
| "ADS1298 SPI 读太慢堵住" | LogicAnalyzer 抓 SPI 波形正常，数据在 RAM 里 |

> ⚠ 写进 `.windsurf/rules/project.md` §7：**任何 `bt_gatt_notify` 的调用方禁止 `(void)` 吞错误**。历史经验显示一旦静默，这类 bug 会反复走弯路。

---

## 4. 根因定位证据

| 阶段 | MTU = 23（Android 默认） | MTU = 247（手动 / bleak） |
|---|---|---|
| ADS1298 SPI 采集 | ✅ 在跑 | ✅ |
| 12-bit 滤波 + 打包成 243 B | ✅ | ✅ |
| `bt_gatt_notify(243 B)` | ❌ `-EMSGSIZE`（单 PDU payload ≤ MTU-3 = 20 B） | ✅ 上链 |
| 错误处理 | `(void)` 吞，无日志 | — |
| 手机收到 | **0 pkt/s** | 100 pkt/s |

**关键线索**：
- 把 `(void)ble_emg_notify(...)` 改成 `LOG_WRN` → RTT 出现密集 `-EMSGSIZE`，立刻锁定根因。
- 同一固件二进制，仅改 central 的 MTU 请求即可复现 / 修复，**与固件 TX 侧策略无关**。

---

## 5. 为什么 HID-only 场景掩盖了这个坑

- HID over GATT 报文 < 20 B，**在 MTU=23 下也能发**。
- OS（iOS / Android）接管 HID 连接时 MTU 常不协商到 247，App 想事后补协商也不一定成功。
- 所以"HID 正常、EMG 全丢"是最容易把人误导到"HID 抢占资源"的现象。
- 真相：两者不是"抢"，是 EMG 从根上就没被 ATT 层放出去。

---

## 6. 解决方案（分两阶段，已全部落地）

### 6.1 Phase A — 设备端主动 MTU 兜底 + 错误可见（2026-05-12 完成）

1. **新增根 `Kconfig`**：`BLE_PERIPHERAL_MTU_EXCHANGE`，默认 `y`；`prj.conf` 启用。
2. **`ble_app.c::mtu_exchange_work_handler`**：连接后 500 ms（给 central 机会先发）检查 `bt_gatt_get_mtu(conn)`：
   - 已 ≥ 246 → 跳过，避免 peripheral 与 central 同时发起 exchange 造成 stack 冲突（iOS / bleak 命中此路径）。
   - 否则 → peripheral 主动 `bt_gatt_exchange_mtu()` 把 MTU 拉到 247（Android 默认命中此路径）。
3. **错误可见**：
   - `emg_acq.c`：`ble_emg_notify(...)` 失败按 1 Hz 限频 `LOG_WRN`，`-ENOTCONN` / `-EAGAIN` 静音（连接 / 背压状态是正常的）。
   - `imu.c`：D1 / D2 / D3 三路 notify 同步加限频 `LOG_WRN`。

### 6.2 Phase B — 启动门控节能（2026-05-12 完成）

1. **`current_mtu` 状态**：由 `gatt_mtu_updated` 回调更新，断开复位到 BLE spec 默认 23。
2. **三重门控**：
   - `ble_emg_notify_enabled()` = CCC 订阅 && 未被 `0xB0` 暂停 && `current_mtu ≥ EMG_PKT_SIZE + 3 (246)`
   - `ble_imu_notify_enabled()` 同理，阈值取最保守的 D3：`IMU_D3_PKT_SIZE + 3 = 86`
3. **零侵入**：`emg_acq` / `imu` 线程本就轮询上述 getter；MTU 不够时自动停 ADS1298 conversion / IMU 采集，省掉"打包后必失败"的无效功耗（ADS1298 2 kHz × 27 B SPI + 滤波 + FPU）。MTU 协商升上去后下一轮 200 ms 轮询自动重启。

---

## 7. App 侧的长期合同（即使设备端已兜底）

> 设备端兜底 ≠ App 可以不做。两边一起做最稳。

- **Android**：连接成功后第一时间 `BluetoothGatt.requestMtu(247)`，等 `onMtuChanged(mtu ≥ 247)` 再 `discoverServices()` → 订阅 CCC。
- **iOS**：CoreBluetooth 自动协商；若 `maximumWriteValueLength(.withoutResponse)` 实测 < 244，EMG 不可用，先用 IMU 验证链路。
- **nRF Connect Android**：手动菜单 "Request MTU" → 247（设备端兜底后，即使不做也能在 500 ms 后收到数据；但显式做更稳、延迟更低）。
- **nRF Connect Desktop / bleak**：无需额外动作，默认就会协商到 247。

详见 [`上位机调试-MTU临时方案.md`](./上位机调试-MTU临时方案.md)。

---

## 8. 验收清单

- [x] RTT 看不到 `-EMSGSIZE` 刷屏（MTU 协商成功后应彻底消失）
- [x] Android nRF Connect **不手动请求 MTU**，连接后 ~500 ms 开始收到 EMG（设备端兜底接管）
- [x] Android nRF Connect 手动请求 MTU 247，连接后立即收到 EMG（central 先手，设备跳过）
- [x] PC bleak（`emg_visualizer_2k.py`）无行为变化，依然稳跑
- [x] EMG 暂停（`B0 01 02`）时 ADS1298 conversion 真的停（功耗有实测下降）
- [ ] iOS 原生 HID + `bleak` 旁路订阅 EMG 的三联 Debug 场景验证（待算法阶段）

---

## 9. 教训与规则沉淀

以下已 / 将写进 `.windsurf/rules/project.md` 与 `architecture.md`，防止后续 AI / 人类重蹈覆辙：

1. **`bt_gatt_notify` 返回值禁止 `(void)` 吞**。失败至少限频 `LOG_WRN`。
2. **BLE 类问题先问三件事**：MTU、PHY、Conn interval。不要上来就怀疑 TX 队列 / 带宽。
3. **平台差异即线索**：同固件在 bleak 正常、Android 不正常 → 大概率是 central 行为差异（MTU / CCCD 时序），不是固件逻辑。
4. **文档同步义务**：MTU 策略变更涉及 `architecture.md §5`、`ble-protocol.md §2`、`上位机调试-MTU临时方案.md`、`.windsurf/rules/project.md`，四处一起改。

---

## 10. 相关 commit / 变更

- `[BLE] 新增 0xB0 流控命令字 + ADS1298 通道级 PGA 增益` — 引入 `0xB0` 掩码机制（早于本次问题发现）
- `[docs] 新增 MTU 临时方案与 IMU Yaw 修复记录 + 物理设计微调` — 首次文档化（临时方案阶段）
- 本次 Phase A/B 合并 commit（待提交）：
  - `src/ble_app.c`：`mtu_exchange_work_handler` 已 ≥247 跳过分支 + `current_mtu` 门控 + `gatt_mtu_updated` 更新 + 断开复位
  - `src/emg_acq.c` / `src/imu.c`：限频 `LOG_WRN`
  - 根 `Kconfig` + `prj.conf`：`CONFIG_BLE_PERIPHERAL_MTU_EXCHANGE=y`
  - `docs/ble-protocol.md`、`docs/architecture.md §5`、本文档同步

---

**维护**：本文档是"已解决问题的历史记录"。不要当设计文档再改；新设计走 `architecture.md`，新协议走 `ble-protocol.md`。Phase B 外还残留的 TBD（CCCD 订阅门控替代自动推流）在 `architecture.md §11.2` Backlog。
