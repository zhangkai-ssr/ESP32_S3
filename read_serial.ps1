param(
    [string]$Port    = "COM13",
    [int]$BaudRate   = 115200,
    [int]$Seconds    = 30,
    [string]$OutFile = "serial_log.txt"
)

$ErrorActionPreference = "Continue"

# Open ASAP after flash hard-reset so we don't miss early boot log
Start-Sleep -Milliseconds 50

$sp = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, "None", 8, "One"
$sp.NewLine     = "`n"
$sp.ReadTimeout = 500
# Disable DTR + RTS BEFORE Open() — those lines map to ESP32 EN/BOOT and
# pulse on port open if left at default true, causing repeated chip resets.
$sp.DtrEnable   = $false
$sp.RtsEnable   = $false

try {
    $sp.Open()
    # Belt-and-suspenders: re-assert disabled after open in case driver
    # auto-enabled them. Also drop any buffered bytes from the reset above.
    $sp.DtrEnable = $false
    $sp.RtsEnable = $false
    $sp.DiscardInBuffer()
} catch {
    Write-Host "ERROR: failed to open $Port - $($_.Exception.Message)"
    exit 1
}

Write-Host "Reading $Port @ $BaudRate for $Seconds seconds -> $OutFile"
"" | Set-Content -Path $OutFile -Encoding UTF8

$end = (Get-Date).AddSeconds($Seconds)
$lineCount = 0
while ((Get-Date) -lt $end) {
    try {
        $line = $sp.ReadLine()
        Add-Content -Path $OutFile -Value $line -Encoding UTF8
        $lineCount++
        # Print at most every 20 lines to keep stdout small
        if ($lineCount % 20 -eq 0) {
            Write-Host "[$lineCount lines]"
        }
    } catch [System.TimeoutException] {
        continue
    } catch {
        Write-Host "read error: $($_.Exception.Message)"
        break
    }
}

$sp.Close()
Write-Host "Done. Captured $lineCount lines to $OutFile"
