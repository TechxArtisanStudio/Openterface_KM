# Map the Host file service as a Windows network drive using the legal
# WebDAV UNC path: \\192.168.20.1@8080\DavWWWRoot
# Run from an elevated PowerShell on the Windows target:
#   powershell -ExecutionPolicy Bypass -File tools\map_webdav_drive.ps1
$ErrorActionPreference = 'Stop'

$davPath = '\\192.168.20.1@8080\DavWWWRoot'
$drive = 'Z:'
$reg = 'HKLM:\SYSTEM\CurrentControlSet\Services\WebClient\Parameters'

# Windows defaults to BasicAuthLevel=1 (HTTPS only). The Host serves plain
# HTTP on the isolated USB link, so allow HTTP basic auth as well.
Set-ItemProperty -Path $reg -Name BasicAuthLevel -Value 2 -Type DWord
if ((Get-Service WebClient).Status -ne 'Running') {
    Start-Service WebClient
} else {
    Restart-Service WebClient
}
Start-Sleep -Seconds 2

net use $drive /delete /y 2>$null | Out-Null
net use $drive $davPath /persistent:yes

Write-Host "Mapped $drive -> $davPath"
Get-ChildItem "$drive\" | Select-Object Name, Length | Format-Table -AutoSize
