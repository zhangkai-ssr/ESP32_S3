#Requires -Version 5.1
<#
.SYNOPSIS
    Claude Code Windows 代理配置脚本（macOS SOP §4-§5 的 Windows 适配版）
.DESCRIPTION
    - 下载 gost（HTTP→SOCKS5 转换器）
    - 通过 Task Scheduler 实现开机自启 + 崩溃自愈（等价 launchd）
    - 持久化写入 ALL_PROXY 用户环境变量
    - 创建健康检查脚本并注入 PowerShell Profile
.NOTES
    使用前：
    1. 填入第 §1 步拿到的 SOCKS5 凭证（HOST/PORT/USER/PASS）
    2. 执行策略若报错，先运行：Set-ExecutionPolicy RemoteSigned -Scope CurrentUser
    3. 不需要管理员权限
#>

# ============================================================
# ★ 填入你的 SOCKS5 凭证（来自 §1 采购的静态 IP，不要提交到 git）
# ============================================================
$SOCKS5_HOST = "71.kookeey.info"
$SOCKS5_PORT = "27126"
$SOCKS5_USER = "a780e1a1"
$SOCKS5_PASS = "26ba060d"
$LOCAL_PORT   = 18080       # 本地监听端口，与 SOP 保持一致

# ============================================================
# 校验占位符
# ============================================================
if ($SOCKS5_HOST -eq "<HOST>" -or $SOCKS5_PORT -eq "<PORT>" -or
    $SOCKS5_USER -eq "<USER>" -or $SOCKS5_PASS -eq "<PASS>") {
    Write-Host "⛔ 请先填入脚本顶部的 SOCKS5_HOST / PORT / USER / PASS，再运行！" -ForegroundColor Red
    exit 1
}

$TASK_NAME   = "GostClaudeProxy"
$GOST_DIR    = "$env:USERPROFILE\.gost"
$GOST_EXE    = "$GOST_DIR\gost.exe"
$CHECK_SCRIPT = "$env:USERPROFILE\gost_check.ps1"

# ============================================================
# §4.1  下载 gost（Windows AMD64）
# ============================================================
if (-not (Test-Path $GOST_EXE)) {
    Write-Host "`n📦 [1/5] 下载 gost..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force -Path $GOST_DIR | Out-Null

    $downloadUrl = $null
    try {
        $release = Invoke-RestMethod "https://api.github.com/repos/go-gost/gost/releases/latest" -UseBasicParsing
        $asset = $release.assets | Where-Object { $_.name -match "windows_amd64" -and $_.name -match "\.zip$" }
        $downloadUrl = $asset.browser_download_url
        Write-Host "  → 版本: $($release.tag_name)" -ForegroundColor DarkGray
    } catch {
        # 回退到已知版本
        $downloadUrl = "https://github.com/go-gost/gost/releases/download/v3.0.0/gost_3.0.0_windows_amd64.zip"
        Write-Host "  → GitHub API 失败，回退到 v3.0.0" -ForegroundColor Yellow
    }

    $zipPath = "$env:TEMP\gost_windows.zip"
    Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -UseBasicParsing
    Expand-Archive -Path $zipPath -DestinationPath $GOST_DIR -Force
    Remove-Item $zipPath -ErrorAction SilentlyContinue

    if (-not (Test-Path $GOST_EXE)) {
        # 有些版本解压后在子目录
        $found = Get-ChildItem -Path $GOST_DIR -Recurse -Filter "gost.exe" | Select-Object -First 1
        if ($found) { Copy-Item $found.FullName $GOST_EXE }
    }

    Write-Host "✅ gost 已安装 → $GOST_EXE" -ForegroundColor Green
} else {
    Write-Host "`n✅ [1/5] gost 已存在，跳过下载：$GOST_EXE" -ForegroundColor Green
}

# 验证二进制可用
& $GOST_EXE -V 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "❌ gost.exe 无法执行，请手动检查：$GOST_EXE" -ForegroundColor Red
    exit 1
}

# ============================================================
# §4.2  创建 Task Scheduler 任务（等价 launchd：开机自启 + 崩溃自愈）
# ============================================================
Write-Host "`n⚙️  [2/5] 注册计划任务 '$TASK_NAME'..." -ForegroundColor Cyan

$GOST_ARGS = "-L http://:$LOCAL_PORT -F socks5://${SOCKS5_USER}:${SOCKS5_PASS}@${SOCKS5_HOST}:${SOCKS5_PORT}"

$action   = New-ScheduledTaskAction -Execute $GOST_EXE -Argument $GOST_ARGS
$trigger  = New-ScheduledTaskTrigger -AtLogon -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit ([System.TimeSpan]::Zero) `
    -RestartCount 3 `
    -RestartInterval (New-TimeSpan -Minutes 1) `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew

Register-ScheduledTask `
    -TaskName $TASK_NAME `
    -Action   $action `
    -Trigger  $trigger `
    -Settings $settings `
    -RunLevel Highest `
    -Force | Out-Null

Write-Host "✅ 任务 '$TASK_NAME' 注册完成（登录自启 + 崩溃重试）" -ForegroundColor Green

# ============================================================
# §4.3  持久化 ALL_PROXY 用户环境变量
# ============================================================
Write-Host "`n🌐 [3/5] 设置 ALL_PROXY 环境变量..." -ForegroundColor Cyan
[System.Environment]::SetEnvironmentVariable("ALL_PROXY", "http://127.0.0.1:$LOCAL_PORT", "User")
$env:ALL_PROXY = "http://127.0.0.1:$LOCAL_PORT"
Write-Host "✅ ALL_PROXY=http://127.0.0.1:$LOCAL_PORT（用户级持久化）" -ForegroundColor Green

# ============================================================
# §4.4  创建健康检查脚本
# ============================================================
Write-Host "`n📋 [4/5] 创建健康检查脚本 → $CHECK_SCRIPT..." -ForegroundColor Cyan

@"
# gost 健康检查 + 自动修复（由 setup_claude_proxy_windows.ps1 生成）
`$port = $LOCAL_PORT
`$taskName = "$TASK_NAME"

`$listening = netstat -ano 2>`$null | Select-String ":\`${port}\s"
if (-not `$listening) {
    Write-Host "⚠️  gost 未运行，正在重启..." -ForegroundColor Yellow
    Start-ScheduledTask -TaskName `$taskName -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    `$listening = netstat -ano 2>`$null | Select-String ":\`${port}\s"
    if (`$listening) {
        Write-Host "✅ gost 已自动重启" -ForegroundColor Green
    } else {
        Write-Host "❌ 重启失败，手动执行：" -ForegroundColor Red
        Write-Host "   Start-ScheduledTask -TaskName `$taskName" -ForegroundColor Yellow
    }
} else {
    Write-Host "✅ gost 运行正常 (port `$port) — Claude Code IP 安全" -ForegroundColor Green
}
"@ | Out-File -FilePath $CHECK_SCRIPT -Encoding utf8 -Force

Write-Host "✅ 健康检查脚本已生成" -ForegroundColor Green

# ============================================================
# 注入 PowerShell Profile（等价写入 ~/.zshrc）
# ============================================================
Write-Host "`n🔧 [5/5] 更新 PowerShell Profile..." -ForegroundColor Cyan

$profileDir = Split-Path $PROFILE -Parent
if (-not (Test-Path $profileDir)) {
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
}
if (-not (Test-Path $PROFILE)) {
    New-Item -ItemType File -Force -Path $PROFILE | Out-Null
}

$currentProfile = Get-Content $PROFILE -Raw -ErrorAction SilentlyContinue
if ($currentProfile -notmatch "gost_check") {
    $profileAddition = @"

# === Claude Code 走美区静态 IP ===
`$env:ALL_PROXY = "http://127.0.0.1:$LOCAL_PORT"
. "`$env:USERPROFILE\gost_check.ps1"
"@
    Add-Content -Path $PROFILE -Value $profileAddition
    Write-Host "✅ Profile 已更新: $PROFILE" -ForegroundColor Green
} else {
    Write-Host "ℹ️  Profile 已有 gost_check，跳过" -ForegroundColor Yellow
}

# ============================================================
# 启动 gost 并执行验证
# ============================================================
Write-Host "`n🚀 启动 gost..." -ForegroundColor Cyan
Start-ScheduledTask -TaskName $TASK_NAME -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
. $CHECK_SCRIPT

Write-Host "`n🔍 验证出口 IP（应与你购买的静态 IP 一致）..." -ForegroundColor Cyan
try {
    $ipInfo = Invoke-RestMethod "https://ipinfo.io" -Proxy "http://127.0.0.1:$LOCAL_PORT" -TimeoutSec 15 -UseBasicParsing
    Write-Host "📍 出口 IP : $($ipInfo.ip)" -ForegroundColor Cyan
    Write-Host "📍 地区    : $($ipInfo.country) / $($ipInfo.region)" -ForegroundColor Cyan
    if ($ipInfo.country -ne "US") {
        Write-Host "⚠️  出口不在美区，请检查代理配置！" -ForegroundColor Red
    } else {
        Write-Host "✅ 美区出口确认" -ForegroundColor Green
    }
} catch {
    Write-Host "⚠️  IP 验证失败（可能代理未就绪）：$_" -ForegroundColor Yellow
    Write-Host "   稍后手动运行：curl https://ipinfo.io" -ForegroundColor DarkGray
}

$line = "=" * 59
Write-Host ""
Write-Host $line -ForegroundColor Cyan
Write-Host "  Windows 代理配置完成！" -ForegroundColor Green
Write-Host $line -ForegroundColor Cyan
Write-Host "  接下来按 SOP 5 完成 Claude Code 安装：" -ForegroundColor White
Write-Host ""
Write-Host "  1. npm install -g @anthropic-ai/claude-code" -ForegroundColor White
Write-Host "     (需要 Node.js 18+, https://nodejs.org)" -ForegroundColor DarkGray
Write-Host ""
Write-Host "  2. 打开新终端(使 ALL_PROXY 生效)，运行：" -ForegroundColor White
Write-Host "     claude" -ForegroundColor Yellow
Write-Host ""
Write-Host "  3. 把弹出的 URL 复制到 AdsPower 浏览器里完成登录" -ForegroundColor White
Write-Host "     不要用系统 Edge / Chrome 打开！" -ForegroundColor Red
Write-Host ""
Write-Host "  验证命令：" -ForegroundColor White
Write-Host "  - netstat -ano | findstr :18080   (检查 gost 进程)" -ForegroundColor DarkGray
Write-Host "  - curl https://ipinfo.io           (验证出口 IP)" -ForegroundColor DarkGray
Write-Host $line -ForegroundColor Cyan
