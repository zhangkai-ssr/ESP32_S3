# nPM1300 长按硬件复位导致 button 线程卡死问题记录

**日期**: 2026-05-15
**症状**: 长按 SHPHLD ~8-9 秒后，button 线程在 `pmic_npm1300_poll_shphld()` 内永久阻塞；偶发整机 warm reset。16s 出厂流程不能触发。

## 现象观察

通过在 `button.c` 加心跳日志 (poll 前/后 trace + 每秒 `BTN hb`) 复现：

```
[00:06:02.871] BTN held from boot_guard, start counting now
[00:06:08.876] Held >=6s -> shutdown preview
[00:06:09.296] BTN hb held=6505ms poll_rc=0
[00:06:10.326] BTN hb held=7540ms poll_rc=0
[00:06:11.351] BTN loop#129 -> poll...     <-- 进入 poll
(此后线程永远不返回, 16s 出厂从不触发)
```

对照测试: 短按释放后, button 线程持续 polling 30+ 秒**没有任何问题**。
所以症状只跟"按住时长"挂钩, 与 wallclock / LED PWM / charger poll 无关。

## 排错过程 (走过的弯路)

| # | 假设 | 验证 | 结果 |
|---|---|---|---|
| 1 | LED PWM 高频 I2C30 写入压垮总线 | 6s 切到无 PWM 的 `LED_MODE_PRE_FACTORY_WHITE_SOLID` | 还是 9s 卡死 |
| 2 | PMIC charger poll 周期事务长 | 暂禁 `pmic_charger_poll_work` | 还是 9s 卡死 |
| 3 | 6s 阈值的 `led_ctrl_set_mode` + `motor_app_feedback_shutdown` 跨线程副作用 | 把 6s 的 LED + motor 调用全砍掉 | 还是 9s 卡死 |
| 4 | nPM1300 硬件长按复位 | 查 DT binding `nordic,npm1300.yaml` | **找到原因** |

## 根本原因

`zephyr/dts/bindings/mfd/nordic,npm1300.yaml` 里 `long-press-reset` 属性的 enum:

```yaml
long-press-reset:
  type: string
  description: Long press reset configuration
  enum:
  - "one-button"   # index 0  ← 默认值 (DT_INST_ENUM_IDX_OR(inst, ..., 0))
  - "disabled"
  - "two-button"
```

当 DT 节点没显式声明 `long-press-reset` 时, MFD 驱动 `mfd_npm1300_init()` 把 enum 索引 0 (`"one-button"`) 写入 `SHIP_BASE + SHIP_OFFSET_LPCONFIG (0x06)`, 启用 nPM1300 的硬件单键长按复位:

- 持续按下 SHPHLD 约 10 秒
- PMIC **复位自身** (重置内部寄存器到 POR)
- LDO1 (给 MCU 供 3.3V) 短暂掉电
- MCU 表现为: I2C30 事务永久挂起 / warm reset / 偶发不重启 (调试器供电)

跟我们的"按住 8-9 秒就出问题"完美吻合。

## 修复

`boards/nrf54l15dk_nrf54l15_cpuapp.overlay` 的 `npm1300:` 节点加一行:

```dts
npm1300: pmic@6b {
    compatible = "nordic,npm1300";
    reg = <0x6b>;
    pmic-int-pin = <0>;
    host-int-gpios = <&gpio1 14 GPIO_ACTIVE_LOW>;
    ship-to-active-time-ms = <1008>;
    long-press-reset = "disabled";   /* 关闭 PMIC 硬件长按复位 */
    ...
};
```

## 为什么我们必须禁用

- 16 秒出厂流程靠**软件**实现 (`button.c` 监 SHPHLD held 时长 → `do_factory_reset()` → `bt_unpair` → cold reboot)
- PMIC 硬件 10s 复位**抢占**软件流程, 让"按 16s 出厂"永远走不到
- 我们也不需要 PMIC 帮我们做"硬件级救命复位": MCU 看门狗 + cold reboot 兜底已经足够

## 后续清理状态

- ✅ `src/pmic_npm1300.c:715` charger poll 周期调度已恢复
- ✅ `src/pmic_npm1300.c:454` GPIO INT 触发的 charger poll 重排已恢复
- ⏳ `src/button.c` 的 `BTN loop#N` / `BTN hb` 诊断日志暂保留 (帮助以后排查); 如要降噪可改 `LOG_DBG`
- ✅ 6s 阈值的 `LED_MODE_PRE_FACTORY_WHITE_SOLID` + 短震反馈已恢复
- ✅ FACTORY_RESET 改回红/白 gamma 平滑脉动 (单边 ~770ms, 全周期 ~1.5s)

## 相关文件

- `boards/nrf54l15dk_nrf54l15_cpuapp.overlay` (DT 修复点)
- `src/pmic_npm1300.c` (临时禁用的 charger poll 待恢复)
- `src/button.c` (诊断日志、6s/16s 阈值逻辑)
- `src/led_ctrl.h` `LED_MODE_PRE_FACTORY_WHITE_SOLID` (6-16s 预览, 无 PWM)
- `zephyr/dts/bindings/mfd/nordic,npm1300.yaml` (DT 属性定义, NCS 自带, 仅供查阅)
- `zephyr/drivers/mfd/mfd_npm1300.c` (写 LPCONFIG 的位置, NCS 自带)
