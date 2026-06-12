# nPM1300 BCHGDISABLE 位锁存导致充电器永久卡在 idle 问题记录

**日期**: 2026-06-12
**症状**: USB 接入后电池不充电；`BCHGCHARGESTATUS` 永远在 `0x00 [idle]` 与 `0x80 [supplement]` 之间切换，`bat_det=0`，所有错误寄存器均为 0。电池端电压保持 3.9V 不变（既不上升也不下降），完全不进入 trickle / CC / CV 任何充电状态。

## 现象观察

5 秒 heartbeat 中 PMIC 状态读取始终如下（电池为新换的 3.9V 满电池，VBUS 正常 5V）：

```
CHG: VBUS=ON BCHG=0x00 [idle]       bat_det=0 err_r=0x00 err_s=0x00
CHG: VBUS=ON BCHG=0x80 [supplement] bat_det=0 err_r=0x00 err_s=0x00
CHG_REGS: ISETMSB=0x25 ISETLSB=0x01 VTERM=0x0D DIS_SET=0x00 NTCR_SEL=0x03
CHG_ADC:  VBAT=3980 mV  VSYS=0 mV   NTC=470 mV  (raw 0xCB/0x00/0x78)
```

- `BCHGCHARGESTATUS` (page 0x03 / offset 0x34) 的 `BatteryDetected` bit 永远为 0
- 即使设置最大充电电流 (ISETMSB=0x7F) 也不能让 charger 离开 idle
- VBAT ADC 读到 3980 mV、NTC ADC 读到 470 mV（≈ 27°C），数值都合理，硬件电气连接正常
- 错误寄存器 `BCHGERRREASON` / `BCHGERRSENSOR` 全 0 —— PMIC 不报任何错误

## 排错过程 (走过的弯路)

| # | 假设 | 验证 | 结果 |
|---|---|---|---|
| 1 | 电池过放 / BMS 锁死 | 换满电 3.9V 新电池 | 现象不变 |
| 2 | VBUS ILIM 默认 100 mA 不够 | 显式写 VBUSINILIM0=1 (500 mA) + TASKUPDATEILIMSW | 现象不变 |
| 3 | ICHG 编码不对 | 写 ISETMSB=0x7F 灌最大充电电流；按知识库公式 `ICHG=MSB*4+LSB*2` 复核 200 mA 编码 | 状态完全不变 |
| 4 | VTERM 4.15V 偏低 | 调到电池 FC 规格 4.20V | 现象不变 |
| 5 | NTC pull-up 阻值与电池不匹配 | 查电池规格书：100 kΩ / β=4250；NTCR_SEL=3 (100kΩ) 匹配 | 配置已对，仍 idle |
| 6 | ADCNTCRSEL page 选错 | 知识库说 page 0x02，Zephyr 用 0x05；用两套 ADC results 反推（VBAT 真值 3.98V 只在 page 0x05 出现） | 确认 page 0x05 才对，原本就对 |
| 7 | EN_SET 单次脉冲没起效 | disable→100ms→enable→50ms→enable 双脉冲 | 现象不变 |
| 8 | **`BCHGDISABLESET` bit 1 跨复位锁存** | 显式写 `BCHGDISABLECLR` (page 0x03 / offset 0x07) = 0x02 | **`bat_det` 立刻 0→1, BCHG 进入 [complete]** ✅ |

## 关键诊断手段

为了把"firmware 配置对不对"和"PMIC 实际行为是什么"分开，在 5s heartbeat 里加了三类信息：

1. **`CHG: ... BCHG=0x?? [phase] bat_det=? err_r=0x?? err_s=0x??`**
   `BCHGCHARGESTATUS` + 错误寄存器读出，让 charger 是否进入工作状态一眼可见。
2. **`CHG_REGS: ISETMSB=.. ISETLSB=.. VTERM=.. DIS_SET=.. NTCR_SEL=..`**
   回读所有刚写过的配置寄存器，证明 I2C 写入是否真正生效（排除"以为写了其实没写"）。
3. **`CHG_ADC: VBAT=.. mV  VSYS=.. mV  NTC=.. mV  (raw ../../..)`**
   PMIC 自己 ADC 测到的 VBAT / NTC / VSYS，证明 PMIC 是否真的"看到了电池"。
   * 注：ADC results 在 **page 0x05** offset 0x10+ (Zephyr `mfd_npm1300`)，不是知识库写的 page 0x02。
   * 用两个 page 同时读，发现只有 page 0x05 给出 3.98V 真值，page 0x02 给出 98 mV 显然不是 VBAT —— 据此确认 ADC 在 page 0x05。

这三行是最后定位根因的关键 —— 如果只看 `BCHGCHARGESTATUS` 一个寄存器，没法分清"是 firmware 没配对"还是"PMIC 没接受配置"。

## 根本原因

nPM1300 的 NTC 监控开关位于 `BCHGDISABLESET` (page 0x03 / offset 0x06) 的 bit 1：

- 写 1 到 `BCHGDISABLESET` bit 1 → **关闭** NTC monitoring
- 写 1 到 `BCHGDISABLECLR` (offset 0x07) bit 1 → **打开** NTC monitoring

**这个 bit 一旦置位会跨 reboot 锁存** —— 哪怕 firmware 后续不再写 disable，bit 也不会回到 0。

历史时间线：

1. 早期固件以为板上 NTC pin 接地（参照 nrf54l15_blinky4 的 reference），写过 `BCHGDISABLESET=0x02` 关闭 NTC monitoring。
2. 后续固件意识到电池真带 NTC，注释里去掉了那一行，期望"默认 NTC enabled"。
3. 但 bit 已经 latch 了 —— `DIS_SET` 寄存器读回是 0x00，因为读的是 `BCHGDISABLESET` 这个 SET 寄存器本身（task register，写后自清），实际生效的内部状态位仍然是 `disabled`。
4. NTC 被锁死在 disabled 状态 → charger 状态机不知道温度 → 永不离开 idle → 不报错（因为不是错误，是"等待温度"）。

新电池 / VBUS / VBAT / NTC / ICHG / VTERM 等所有外围条件其实都正确，PMIC 只是在等一个永远不会被打开的内部位。

## 解决方案

`npm1300_enable_charger()` 在 `BCHGENABLESET` 之前，显式写一次 `BCHGDISABLECLR=0x02` 清掉锁存位：

```c
/* Explicitly RE-ENABLE NTC monitoring via BCHGDISABLECLR bit 1.
 * A previous firmware build set BCHGDISABLESET bit 1 to disable NTC;
 * that bit can stay latched across resets, so we have to clear it
 * explicitly rather than rely on the power-on default. */
ret = npm1300_write(NPM1300_CHGR_PAGE, NPM1300_BCHG_DIS_CLR, 0x02);
```

修复 commit: `5494c18` ("fix(pmic): explicitly clear BCHGDISABLE bit 1 — charger now leaves idle")。

## 验证

修复后第一次启动：

```
CHG: VBUS=ON BCHG=0x03 [complete]   bat_det=1 err_r=0x00 err_s=0x00
CHG: VBUS=ON BCHG=0x81 [supplement] bat_det=1 err_r=0x00 err_s=0x00
CHG: VBUS=ON BCHG=0x03 [complete]   bat_det=1 err_r=0x00 err_s=0x00
```

- `bat_det` 从 0 立刻翻成 1 —— PMIC 第一次承认电池存在
- `BCHG` 进入 `0x03 = BatteryDetected | ChargingComplete` —— 因为测试电池 3.98V 已经接近 VTERM=4.20V，charger 判定满电立即终止
- 在 supplement 与 complete 之间切换是正常行为（满电后 PMIC 用 VBAT 协助 VSYS 是设计行为）

换一块电压更低（比如 3.6V）的电池可以观察到 trickle/CC/CV 全过程。

## 教训

1. **PMIC 上"SET / CLR"寄存器对要成对使用**。nPM1300 的 `*ENABLESET` / `*ENABLECLR` 与 `*DISABLESET` / `*DISABLECLR` 都是 task register 风格 —— 写 1 触发动作然后自清，**读回这些 register 拿不到状态**。要清掉之前置过的位必须显式写对应的 CLR 寄存器，不能假设"重启后默认"。
2. **register dump heartbeat 比一次性 boot dump 有用得多**。host 串口同步延迟会吃掉 boot 阶段最前面 1~2 秒的日志，把 register dump 挪到 5s 心跳里之后一次能看到完整状态，定位时间从"猜+重 flash"降到"看一眼"。
3. **不报错不等于 firmware 正确**。这次 PMIC 全程 `err=0/0`，是因为 PMIC 把"等待温度数据"当作正常等待，不是错误。只看错误寄存器会被误导，必须读完整的 `BCHGCHARGESTATUS` 配合 ADC 自测值交叉验证。
4. **不要无脑信知识库 / 参考项目里的 register page**。
   - 知识库说 ADCNTCRSEL 在 page 0x02，实测在 page 0x05；
   - 参考项目 `nrf54l15_blinky4` 配置的是"NTC 接地、disable monitoring"的板子，照搬到带真 NTC 的电池板上就是 bug 源头。
   交叉验证（多个 page 都读、对照 ADC 真值）是发现这类不一致的有效手段。

## 后续观察 — 间歇性 BATTERY_DETECT 失败（硬件层）

**日期**: 2026-06-12（修复 commit `5494c18` 当天）

修复 commit 上线后 charger 第一次工作（trickle / complete / supplement 都正常切换），但**同一块板同一颗电池连续两次复位**得到不同结果：

| | 第一次 RST | 第二次 RST |
|---|---|---|
| Boot 2160ms `BCHG_STATUS` | **0x05** (bat=1, trickle 启动) ✅ | **0x80** (bat=0, supplement) ❌ |
| 后续充电状态 | trickle 跑了 155s 后才退化到 `bat_det=0` | 启动即 `bat_det=0`，永远不进入 trickle |
| `VBUS_STATUS` | 0x21 | **0x23** (bit 不同) |
| VBAT ADC | 3980 mV | 3980 mV |
| NTC ADC | 466 mV | 466 mV |
| 所有 init 寄存器 readback (`DIS_SET`=0x00, `NTCR_SEL`=0x03, `ISETMSB`=0x32, `VTERM`=0x0E, …) | 全部正确 | 全部正确 |

Firmware 配置两次完全一致，但 PMIC 一次能完成 BATTERY_DETECT、一次不能 — 间歇性，**不是 firmware 能控制的**。

### 现象解读

PMIC 通过 ADC 走高阻路径能读到稳定的 VBAT = 3.98V，但 charger 启动时需要往电池注入一个测试电流脉冲（BATTERY_DETECT task），这一步对**电池支路的真实交流阻抗 / 接触电阻**敏感。如果支路上某处接触电阻偏大或抖动：

- ADC 静态读电压不受影响（高阻输入，nA 级电流）
- charger 注入 200 mA 测试电流时端电压瞬间被拉高 → PMIC 判定"电池接受电流能力不足" → BATTERY_DETECT 失败 → 状态机停在 idle / supplement，**不报错**（因为不是错误是判断结果）

这跟我们看到的现象完全吻合：err 全 0、ADC 数值正常、唯一 `bat_det=0`。

### 怀疑点（按概率排序）

| 嫌疑点 | 验证方法 |
|---|---|
| 电池 JST 连接器虚接 / 触点氧化 | 拔出端子用酒精棉擦干净，重新插紧后再测；或者轻按连接器观察 `BCHG_STATUS` heartbeat |
| PCB 上 JST 母座 4 个焊脚虚焊 | 烙铁过一遍补焊；插拔几次看是否结果稳定 |
| PMIC `BAT` pin 焊脚虚焊 | 显微镜 / 烙铁补焊 PMIC `BAT` 和邻近 GND pin |
| PCB 上电池支路串了限流电阻 / 0 Ω 跳线脱落 | 万用表测电池正极 → PMIC `BAT` pin 通路 |

### Firmware 边界

`npm1300_enable_charger()` 已经做了所有能做的：

- VBUS ILIM 500 mA + TASKUPDATEILIMSW
- BCHGDISABLECLR=0x02 显式 enable NTC monitoring（修了上面那个根因）
- NTCR_SEL=0x03 (100kΩ pull-up) 匹配电池规格 (YJ503030, 100kΩ β=4250)
- ICHG=200 mA, VTERM=4.20V（电池数据手册标准充电参数）
- disable → 配置 → 100ms 延迟 → ERR_CLR → EN_SET → 50ms → 再 EN_SET

每个寄存器 init 后都立即 readback 验证。这块没有进一步可改的余地。

### 结论

这个 issue **不归 firmware 管**，留给硬件团队带烙铁排查电池支路接触。Firmware 这边把 `BCHG_STATUS` / `VBUS_STATUS` / VBAT / NTC ADC 放进 heartbeat（详见 commit `02669e0` 的 lazy 版本），硬件团队可以直接看串口判断接触是否恢复。
