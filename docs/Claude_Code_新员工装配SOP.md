# Claude Code 新员工装配 SOP

> **适用**：新同学入职当天，从零把 Claude Code 跑起来。
> **底层逻辑**：Claude 账号 ↔ AdsPower 指纹浏览器 ↔ 美区静态 IP ↔ CLI 出口，**四位一体**必须全程锁死在同一个身份上，任何一端漂移都会封号。
> **预计耗时**：2–3 小时（含视频教程）。

---

## 0 · 架构总览（先对齐脑图再动手）

```
┌─────────────────────┐       ┌─────────────────────┐
│ AdsPower 指纹浏览器    │──┐    │ claude CLI（终端）    │
│  · Claude 账号登录    │  │    │                     │
│  · 美区 PayPal 订阅   │  │    └──────────┬──────────┘
└─────────────────────┘  │               │ ALL_PROXY=http://127.0.0.1:18080
                         │               ▼
                         │    ┌─────────────────────┐
                         │    │ gost 本地 :18080     │ ← launchd 常驻
                         │    │  HTTP → SOCKS5 转换  │   开机自启 + 崩溃自愈
                         │    └──────────┬──────────┘
                         │               │ socks5
                         ▼               ▼
                  ┌──────────────────────────────┐
                  │ 美区住宅静态 IP（kookeey 等）  │  ← 唯一出口
                  └──────────────────────────────┘
```

**一句话抓手**：浏览器出口 IP = CLI 出口 IP = 你买的那个静态 IP。三者相等 = 安全；任何不等 = 账号随时可能挂。

---

## 1 · 采购美区静态 IP（自购 / 报销）

- **推荐供应商**：kookeey（美区住宅 SOCKS5，稳定可复用）。其他等价供应商也行，但必须满足：
  - 住宅 IP（非机房 IP）
  - 美国（与 Claude 账号区域一致）
  - **静态**（非轮换）
  - 支持 **SOCKS5** 协议
- 购买后你会拿到 4 个值，妥善记录：`HOST / PORT / USER / PASS`
- **到期日**立刻加飞书日历，**提前 7 天**设提醒。过期 = gost 断链 = Claude 全线报错。

> ⚠️ 自购凭证不要贴群、不要进 git、不要塞进任何公共文档。

---

## 2 · 装 AdsPower 指纹浏览器 + 注册 Claude 账号

### 必看视频（跟完再动手）

📺 **【指纹浏览器+环境搭建全流程教学，claude/美区paypal防封号必备！】**
https://www.bilibili.com/video/BV1uqBsBUEDM/

视频覆盖：AdsPower 安装、环境建档、美区 PayPal 开通、Claude 账号注册与订阅付款全流程。

### 关键动作

1. 下载 AdsPower → 新建"浏览器环境"
2. **代理设置**里选 `SOCKS5`，把第 1 步的 `HOST / PORT / USER / PASS` 全部填进去
3. 环境参数全部按美国设置：**时区** US、**语言** en-US、**UA** 美版、**分辨率**保持默认
4. 启动环境 → 在这个 AdsPower 浏览器里完成：
   - Claude 账号注册（Google 登录也行，但 Google 必须也是美区号）
   - 美区 PayPal 绑卡
   - Claude Pro / Max 订阅付款

> **红线**：Claude 账号**终生只能**在这个 AdsPower 环境里打开。不准用 Safari / Chrome 原生浏览器登录，一次也不行。

---

## 3 · 装 Clash（桌面日常流量，和 Claude 完全无关）

- 装 **Clash Verge** 或 **ClashX Pro**，导入公司机场订阅
- 打开 **TUN Mode**（系统级接管）
- **规则里显式排除 Claude 域名**，让它们走系统默认出口而不是机场节点：

  ```yaml
  # 示例 rule（放规则组靠前）
  - DOMAIN-SUFFIX,anthropic.com,DIRECT
  - DOMAIN-SUFFIX,claude.ai,DIRECT
  ```

**动机**：Clash 机场 IP 是共享的、会漂的，绝对不能让 Claude 走它。Clash 只给你刷 GitHub / YouTube 用。

---

## 4 · shell 里装 gost（让 CLI 强制走静态 IP）

### 4.1 安装 gost

```bash
brew install gost
gost -V        # 期望输出 gost 3.x
```

### 4.2 建 launchd 常驻配置

把 `<HOST> <PORT> <USER> <PASS>` 替换成第 1 步的实际值后，整段粘到终端：

```bash
cat > ~/Library/LaunchAgents/com.user.gost-kookey.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.user.gost-kookey</string>
    <key>ProgramArguments</key>
    <array>
        <string>/opt/homebrew/bin/gost</string>
        <string>-L</string>
        <string>http://:18080</string>
        <string>-F</string>
        <string>socks5://<USER>:<PASS>@<HOST>:<PORT></string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>StandardOutPath</key><string>/tmp/gost-kookey.log</string>
    <key>StandardErrorPath</key><string>/tmp/gost-kookey.log</string>
</dict>
</plist>
EOF

launchctl load ~/Library/LaunchAgents/com.user.gost-kookey.plist
```

**这一步的闭环含义**：
- gost 在本地 `:18080` 监听 HTTP 代理
- 所有进入 `:18080` 的流量被转成 SOCKS5，发给你的美区静态 IP
- launchd 负责 **开机自启 + 崩溃自动重启**，你 shell 什么都不用手动拉

### 4.3 让 CLI 走 gost

编辑 `~/.zshrc`，在末尾追加：

```bash
# === Claude Code 走美区静态 IP ===
export ALL_PROXY=http://127.0.0.1:18080

# 每次开终端做一次 gost 健康检查
bash ~/gost-check.sh
```

### 4.4 写健康检查脚本

```bash
cat > ~/gost-check.sh <<'EOF'
#!/bin/bash
# gost 健康检查 + 自动修复
if ! lsof -i :18080 -sTCP:LISTEN &>/dev/null; then
    echo "⚠️  gost 未运行，正在重启..."
    launchctl kickstart -k "gui/$(id -u)/com.user.gost-kookey" 2>/dev/null
    sleep 2
    if lsof -i :18080 -sTCP:LISTEN &>/dev/null; then
        echo "✅ gost 已自动重启"
    else
        echo "❌ 重启失败，手动执行："
        echo "   launchctl load ~/Library/LaunchAgents/com.user.gost-kookey.plist"
    fi
else
    echo "✅ gost 运行正常 (port 18080) — Claude Code IP 安全"
fi
EOF
chmod +x ~/gost-check.sh
```

重启一次终端，应看到 `✅ gost 运行正常`。

---

## 5 · 装 Claude Code 并登录

```bash
# 需要 Node 18+
npm install -g @anthropic-ai/claude-code

# 让 ALL_PROXY 生效
source ~/.zshrc

# 首次登录
claude
```

登录流程：
1. `claude` 会弹一段 URL
2. **复制这段 URL → 粘到第 2 步的 AdsPower 浏览器里打开**
3. 在 AdsPower 里完成登录 → 拿到 token
4. 粘回终端

> **红线**：登录 URL 不许在 Safari / 系统 Chrome 里打开。一次都不行。

---

## 6 · 闭环验证（6 项全绿才算装配完成）

| # | 检查项 | 命令 / 动作 | 期望结果 |
|---|---|---|---|
| 1 | gost 进程存活 | `lsof -i :18080` | 有一行 gost LISTEN |
| 2 | CLI 出口 IP | `curl https://ipinfo.io` | 美区 IP = 你买的静态 IP |
| 3 | AdsPower 出口 IP | AdsPower 浏览器访问 `https://ipinfo.io` | 与第 2 项完全一致 |
| 4 | Claude 能调通 | `claude` 交互里随便问一句 | 正常响应，无 403 / 429 |
| 5 | 桌面出口 ≠ 静态 IP | 系统 Safari 访 `https://ipinfo.io` | 是机场节点 IP（不等于静态 IP） |
| 6 | 订阅状态 | AdsPower 里访问 claude.ai/settings | 显示有效的 Pro / Max |

**前 5 项任一 fail = 装配失败，不允许开始交付任何代码工作。**
装完后将第 2 + 第 3 项的 `ipinfo.io` 截图发到 `#claude-code-装配` 飞书群，由 IT 或 mentor 验收。

---

## 7 · 日常运维

| 场景 | 动作 |
|---|---|
| 终端提示 `⚠️ gost 未运行` | 脚本会自动重启；若仍失败：`launchctl load ~/Library/LaunchAgents/com.user.gost-kookey.plist` |
| Claude 突然 403 / 429 | 先跑 `curl https://ipinfo.io` 看出口 IP 对不对；再检查订阅有没有过期 |
| 换电脑 | 重走 §4、§5；kookeey 账号**不要注销**，延续原静态 IP |
| 出差 / 换 WiFi | gost 不受本地网络影响；若 kookeey 限源 IP 才需要联系 IT 调整白名单 |
| 静态 IP 快到期 | 提前 7 天飞书提醒 → 续费；**不要**先让它断，再续费后 IP 可能会变 |

---

## 8 · 安全红线（背下来）

1. Claude 账号**只在 AdsPower 环境里登录**，系统浏览器终生禁入
2. CLI **只通过 gost 出口**，`ALL_PROXY` 不许删、不许改
3. 静态 IP / kookeey 凭证 / API key **不许贴群、不许进 git、不许写任何公共文档**
4. 一个人的 AdsPower 环境 / 静态 IP / Claude 账号**不共享**——串号 = 集体封号
5. 发现 gost 挂了 / IP 漂了，**立即停止使用 claude**，修好再开工

---

## 9 · 出问题找谁

- **装配本身卡住** → 飞书 `#claude-code-装配` 群
- **kookeey 账号 / 静态 IP** → 报销流程走 IT，续费走行政
- **AdsPower 环境异常** → 同 IT
- **Claude 订阅 / 付款** → 财务（走美区 PayPal 报销流程）

---

> **完成标志**：第 6 节 6 项全绿 + 飞书群截图验收通过 = 你可以开始用 Claude Code 了。
