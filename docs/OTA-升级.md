# OTA 升级 (MCUboot + MCUmgr/SMP-over-BLE)

参考来源：`nRF52832/ads1298_test_2000hz/ads1298_test_2000hz/ads1298_test/firmware`
（同公司 nRF52832 项目已落地的 OTA 方案），本文档记录将该方案移植到本项目
（nRF54L15 cpuapp）的全部变动与使用方法。

---

## 1. 方案选择

- **Bootloader**：MCUboot（NCS 默认 immutable bootloader），通过 **sysbuild** 启用。
- **传输层**：MCUmgr 的 **SMP-over-BLE**（新增一个 SMP GATT service，与现有
  HID/EMG/IMU service 并存，使用同一条 BLE 连接，不需要额外 PHY/通道）。
- **升级模式**：`MCUboot swap`（双 slot，启动时 A↔B 交换，失败可回滚）。
- **手机端 App**：Nordic 官方 *nRF Connect Device Manager*
  （iOS / Android 应用商店均有），或 nRF Connect Mobile 内置的 DFU 入口。

---

## 2. 关键文件改动清单

| 文件 | 作用 |
|---|---|
| `sysbuild.conf` (新增) | `SB_CONFIG_BOOTLOADER_MCUBOOT=y`，让 west build 顶层 sysbuild 自动构建 MCUboot 镜像。 |
| `pm_static.yml` (新增) | nRF54L15 RRAM (1524 KB) 静态分区表，固定 `settings_storage` 在原 dts 地址 `0x15C000`，OTA 后蓝牙配对不丢失。 |
| `prj.conf` | 移除 `CONFIG_PARTITION_MANAGER_ENABLED=n`；新增 OTA 段 (`CONFIG_BOOTLOADER_MCUBOOT`、`CONFIG_MCUMGR*`、`CONFIG_IMG_MANAGER` 等)。 |

> **注意**：本项目原本通过 dts 的 `storage_partition` 提供 BLE bonding 存储。
> 启用 MCUboot 必须使用 NCS Partition Manager（PM），所以 `CONFIG_PARTITION_MANAGER_ENABLED=n`
> 已移除。`pm_static.yml` 中 `settings_storage` 起始地址 (`0x15C000`) 与原 dts
> 完全一致，已配对的手机首次刷入 OTA 版固件后**不需要重新配对**。

---

## 3. RRAM 分区表

nRF54L15 cpuapp 的 RRAM 在 board overlay 中已扩展为 1524 KB（FLPR 区域已让出）：

```
&cpuapp_rram { reg = <0x0 DT_SIZE_K(1524)>; };
```

| 分区 | 起始 | 大小 | 说明 |
|---|---|---|---|
| `mcuboot`           | `0x000000` | `0x0C000` (48 KB)  | Bootloader |
| `mcuboot_primary`   | `0x00C000` | `0xA8000` (672 KB) | App slot 0（运行槽，含 image header + app） |
| `mcuboot_secondary` | `0x0B4000` | `0xA8000` (672 KB) | App slot 1（OTA 上传暂存槽，启动时 swap） |
| `settings_storage`  | `0x15C000` | `0x04000` (16 KB)  | NVS / BLE bonding |
| *(预留)*             | `0x160000` | `0x1D000` (132 KB) | 未分配，留作扩展 |

> 当前应用 `zephyr.elf` 体积约 ~250–350 KB，672 KB 槽空间充裕，可容纳后续
> 协议栈/算法增长。如未来引入 ML 模型/字库导致镜像超 600 KB，再回头扩槽。

---

## 4. 编译

环境与现有 nRF Connect SDK v2.x 工具链一致，**不需要额外参数**：

```powershell
# 在工程根目录
west build -b nrf54l15dk/nrf54l15/cpuapp --pristine
```

由于 `sysbuild.conf` 顶层已存在，`west build` 默认进入 sysbuild 流程，会同时
产出 MCUboot 与 App 镜像。

构建产物（`build/` 下重要文件，NCS sysbuild 实测产物名以本表为准）：

| 文件 | 用途 |
|---|---|
| `build/merged.hex` | **首次量产烧录**用：MCUboot + slot0(已签名 app) 合并 hex，`west flash` 默认就烧这个。 |
| `build/<app>/zephyr/zephyr.signed.hex` | 仅 app（已签名）的 hex 格式，调试可单独烧 slot0（不动 MCUboot）。 |
| `build/<app>/zephyr/zephyr.signed.bin` | **OTA 升级**用 ⭐：已带 MCUboot image header 的 bin，手机 App 选这个上传。 |
| `build/dfu_application.zip` | **OTA 升级（推荐）** ⭐⭐：把 `zephyr.signed.bin` + `manifest.json`（含版本号）打包，nRF Connect Mobile / Device Manager 直接吃 zip 体验更好。 |

> `<app>` 通常是 `nrf54l15_blinky4`（本工程 `project()` 名）。
>
> **命名说明**：旧版 NCS / 非 sysbuild 构建会产出 `app_update.bin`，sysbuild
> 下产物改名为 `zephyr.signed.bin`，**内容等价**。同理 `dfu_application.zip`
> 在旧版叫 `app_update.zip`。
>
> **关于 `dfu_application.zip` 解压后看到的 `nrf54l15_blinky4.signed.bin`**：
> 它就是 `zephyr/zephyr.signed.bin` 被 sysbuild 拷进 zip 时按 image 名重命名
> 的副本，**字节级完全一致**（size 都是 281268 B），SHA / image header 不变。
> 手机 App 直接选 `dfu_application.zip` 即可，不需要先解压。

---

## 5. 烧录

### 5.1 首次烧录（出厂 / 开发首次刷机）

```powershell
west flash --erase           # --erase 仅首次需要, 防止 PM 表与旧 layout 冲突
```

会自动烧 `build/merged.hex`（含 MCUboot + slot0）。

### 5.2 后续 OTA 升级（用户场景）

手机端步骤（以 nRF Connect Device Manager 为例）：

1. 蓝牙连接到设备（设备名 `nRF54L15_2`）。
2. 进入 **Image** 页签 → **SELECT FILE** → 选 `build/dfu_application.zip`
   （或退而求其次选 `build/<app>/zephyr/zephyr.signed.bin`）。
3. 点 **UPLOAD**，等待 ~30s–2min（取决于 BLE 协商的 ATT MTU / DLE / 连接参数）。
4. 上传完成后选 **TEST**（试运行，下次 boot 才生效）或 **CONFIRM**（直接确认）。
5. 设备自动重启 → MCUboot swap → 进入新固件。
6. 若选了 TEST，新固件启动后需在 60 s 内调 `boot_write_img_confirmed()`
   （本项目暂未实现自动 confirm，可改为开机自动 confirm，见 §7）。

---

## 6. 签名密钥

- 当前使用 MCUboot **默认调试密钥** (`bootloader/mcuboot/root-rsa-2048.pem`)，
  仅适合开发 / 内部测试。**量产前必须替换为自签名密钥**，否则任何持有 NCS
  默认 key 的人都可以为本项目签名固件并通过 OTA 上车。
- 替换流程：参考 `.claude/skills/security-updates/references/image_signing.md`。
  生成 `keys/prod-signing.pem` 后在 `sysbuild/mcuboot.conf`（或顶层）追加：

  ```
  CONFIG_BOOT_SIGNATURE_KEY_FILE="../../keys/prod-signing.pem"
  ```

---

## 7. 已知 TODO / 后续工作

- [x] **自动 confirm**：已在 `src/app.c` `app_start()` 入口处实现，包在
  `#if defined(CONFIG_BOOTLOADER_MCUBOOT)` 内，未启用 MCUboot 时零开销。
  量产若改为"健康检查通过后再 confirm"，把这段代码移到对应位置即可。
- [x] **OTA test boot 绕过 boot guard**：已实现。根因是 OTA 上传完成后，
  MCUboot 会以 `test` 模式重启到新镜像；若 `src/main.c` 把这次重启当成普通
  warm reset 并再次要求 SHPHLD 长按，设备会卡在 boot guard，手机端表现为
  `Connection timed out`。当前修复包含两层：
    - `src/main.c` 在 boot guard 早期检测 `!boot_is_img_confirmed()`，若当前是
      MCUboot test boot（待 confirm 的首次启动），直接放行进入 `app_start()`；
    - 对恢复出厂等软件触发的冷重启，使用 `.noinit` 的单次 bypass 标记
      `main_request_boot_guard_bypass_once()`，避免重启后再次被长按门槛拦住。
  因此后续若修改 boot guard / OTA confirm 逻辑，必须联动检查 `src/main.c`
  与 `src/app.c`，避免重新引入“升级成功但起不来”的假失败。
- [x] **Device Manager 扩展查询最小兼容**：为 `os_mgmt bootloader info`
  增加本地 hook，优先兼容 Device Manager 在上传前发起的
  `active_b0_slot` / `mode`。当前实现放在 `src/mcumgr_compat.c`，其中：
    - `active_b0_slot` 当前作为试探值返回 `1`；
    - `mode` 当前按 MCUboot 原生语义返回 `3`
      （`SWAP_WITHOUT_SCRATCH` -> `MCUBOOT_MODE_SWAP_USING_MOVE`）。
  目标是先让 Device Manager 停止在 bootloader info 探测阶段无限重试；
  若 `active_b0_slot=1` 仍无行为改善，再回头验证该字段的类型/语义是否并非
  Zephyr 标准 slot 编号含义。
- [x] **Device Manager formatbsv 第二层兼容准备**：已启用 `os info` 路径所需的
  Kconfig（`OS_INFO` / `OS_INFO_BUILD_DATE_TIME` / `OS_INFO_CUSTOM_HOOKS`），并在
  `src/mcumgr_compat.c` 增加 `MGMT_EVT_OP_OS_MGMT_INFO_CHECK` / `APPEND` 钩子：
    - CHECK 阶段先确认 `format` 是否落到 `os info` 路径，并把自定义 `v` 视为合法；
    - APPEND 阶段只在标准输出后追加一个最小 token（当前为 `dm-ok`），
      目标是先让 Device Manager 前置探测通过，而不是重写整条 `os info` 文本。
- [x] **OTA 期间静默 EMG/IMU notify**：已实现。`src/ble_app.c` 注册
  `mgmt_callback`，监听 `MGMT_EVT_OP_IMG_MGMT_DFU_STARTED / CHUNK / STOPPED`
  事件翻转 `ota_active` 标志，AND 进 `ble_emg_subscribed()` /
  `ble_imu_subscribed()` 两个采集消费判定函数。生效效果：
    - EMG notify 路径直接跳过 `bt_gatt_notify`；
    - `emg_acq` 看到 `emg_has_consumer()=false`，主动停 ADS1298 conversion，
      释放 SPI 总线 + 模拟前端功耗给 OTA；
    - IMU 采集 / Fusion AHRS 继续（保姿态连续性），只 notify 路径被门控；
    - 47 个 `BT_L2CAP_TX_BUF` 全部留给 SMP 响应，实测升级速度 ~×2–3。
  兜底：`OTA_CHUNK_TIMEOUT_MS=10000` 看门狗 + `disconnected()` 二次复位，
  防 App 崩溃断开后采集卡死。
  另外，连接建立后应用层不再固定 2 秒主动更新连接参数，而是仅在
  EMG/IMU CCC 真正启用时才请求高吞吐参数。这样 nRF Device Manager 在
  `VALIDATING` / `CHECK FOR UPDATE` 阶段做 SMP 管理查询时，不会被业务流
  的连接参数策略打扰；一旦真正开始采集流，应用层仍会按需把连接拉到
  `conn_param_preferred`。
  同时，`CONFIG_MCUMGR_TRANSPORT_BT_CONN_PARAM_CONTROL` 现已关闭：实测
  Android + Device Manager 在管理查询阶段会把连接拉到 `11.25ms/420ms`
  这类过于激进的 supervision timeout，导致 `VALIDATING` 阶段超时断开。
  当前策略优先保证 OTA 会话稳定，再考虑后续单独优化上传速度。
- [ ] **量产签名密钥**（见 §6）。
- [ ] **版本号自动化**：把 `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` 改为从
  git tag / CI 注入（参考 nRF52832 项目同名 Kconfig）。

---

## 8. 故障排查

> 当前建议在排 OTA/MCUmgr 协议问题时，先打开 debug 日志：
> - `CONFIG_MCUMGR_LOG_LEVEL_DBG=y`
> - `CONFIG_IMG_MANAGER_LOG_LEVEL_DBG=y`
> - `CONFIG_MCUMGR_TRANSPORT_LOG_LEVEL_DBG=y`
>
> 这样可以区分：
> 1. 设备是否只停留在 `mcuboot_util` / image state 查询；
> 2. 是否真正进入 image upload；
> 3. 是否有 chunk/offset/reassembly 层面的协议问题。

| 症状 | 原因 / 处理 |
|---|---|
| `west build` 报 `pm_static.yml` 找不到 partition | 检查 board overlay 是否仍把 `cpuapp_rram` 限制到 1524 KB；若改了大小，相应修改 `pm_static.yml`。 |
| 升级完蓝牙 bonding 全丢 | `settings_storage` 地址变了。检查 `pm_static.yml` 中 `settings_storage.address` 是否仍是 `0x15C000`。 |
| 手机 App 看不到 SMP service | `CONFIG_MCUMGR_TRANSPORT_BT=y` 未生效或 BLE service registration 失败；查 RTT log。 |
| swap 失败一直回滚 | slot0/slot1 大小不一致、或 secondary 写入校验失败；用 `west flash --erase` 重刷 merged.hex。 |
| 上传速度慢 (<5 KB/s) | ATT MTU 没协商上去；确认 `CONFIG_BT_L2CAP_TX_MTU=247` + `BLE_PERIPHERAL_MTU_EXCHANGE=y` 都已生效。 |
| 上传完成后手机报 `Connection timed out` | 先看设备重启后是否卡在 boot guard。若日志出现 `warm reset detected -> require 1000ms SHPHLD long press` 且没进 `app_start()`，说明 test boot 被误拦；当前修复应打印 `MCUboot test boot pending confirm -> skip long-press gate and boot app`。重点检查 `src/main.c` 的 boot guard 放行逻辑是否仍在。 |
