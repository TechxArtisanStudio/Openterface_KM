# Auto-open the KeyMod file manager when the device is plugged in.
# Installs a background watcher + scheduled task on the Windows target:
#   powershell -ExecutionPolicy Bypass -File tools\target_autopen_browser.ps1
# The watcher polls for the NCM adapter (192.168.20.2); on plug-in it waits
# for the Host to answer, then opens the default browser at the file
# manager. One popup per plug-in event.

$ErrorActionPreference = 'Continue'
$installDir = 'C:\ProgramData\KeyMod'
$watcherPath = Join-Path $installDir 'autopen_watcher.ps1'
$logPath = Join-Path $installDir 'autopen.log'
$taskName = 'KeyModFileAutoOpen'
$url = 'http://192.168.20.1:8080/'

# ---------- 1) install the watcher script ----------
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
$watcher = @'
$installDir = 'C:\ProgramData\KeyMod'
$logPath = Join-Path $installDir 'autopen.log'
$url = 'http://192.168.20.1:8080/'
$lastUp = $false
while ($true) {
    try {
        $ip = Get-NetIPAddress -IPAddress '192.168.20.2' -AddressFamily IPv4 -ErrorAction SilentlyContinue
        $up = [bool]$ip
        if ($up -and -not $lastUp) {
            $ok = $false
            foreach ($i in 1..12) {
                if (Test-Connection -ComputerName 192.168.20.1 -Count 1 -Quiet) { $ok = $true; break }
                Start-Sleep -Seconds 1
            }
            if ($ok) {
                Start-Process $url
                Add-Content -Path $logPath -Value ("{0} opened {1}" -f (Get-Date -Format s), $url)
            } else {
                Add-Content -Path $logPath -Value ("{0} adapter seen but Host unreachable" -f (Get-Date -Format s))
            }
        }
        $lastUp = $up
    } catch {
        Add-Content -Path $logPath -Value ("{0} ERROR {1}" -f (Get-Date -Format s), $_.Exception.Message)
    }
    Start-Sleep -Seconds 3
}
'@
Set-Content -Path $watcherPath -Value $watcher -Encoding UTF8
Write-Host "WATCHER_INSTALLED $watcherPath"

# ---------- 2) register the scheduled task (runs at logon, in session) ----------
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$watcherPath`""
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
    -Principal $principal -Description 'Open the KeyMod file manager when the device is plugged in' `
    -Force | Out-Null
Write-Host "TASK_REGISTERED $taskName"

# ---------- 3) start it now ----------
Start-ScheduledTask -TaskName $taskName
Start-Sleep -Seconds 6
$state = (Get-ScheduledTask -TaskName $taskName).State
Write-Host "TASK_STATE $state"
Get-Content $logPath -ErrorAction SilentlyContinue | Select-Object -Last 5
