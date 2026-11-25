# Enable Windows Network discovery for the KeyMod NCM adapter.
# Run from an elevated PowerShell on the Windows target:
#   powershell -ExecutionPolicy Bypass -File tools\enable_windows_discovery.ps1
$ErrorActionPreference = 'Stop'

$targetIp = '192.168.20.2'
$nic = Get-NetIPAddress -AddressFamily IPv4 -IPAddress $targetIp -ErrorAction SilentlyContinue
if (-not $nic) {
    Write-Warning "Adapter with $targetIp not found - is the KeyMod target port connected?"
    exit 1
}

$conn = Get-NetConnectionProfile -InterfaceIndex $nic.InterfaceIndex
Write-Host "Adapter $($conn.InterfaceAlias): $($conn.NetworkCategory)"
if ($conn.NetworkCategory -ne 'Private') {
    Set-NetConnectionProfile -InterfaceIndex $nic.InterfaceIndex -NetworkCategory Private
    Write-Host "Switched $($conn.InterfaceAlias) to Private"
}

foreach ($svc in @('fdPHost', 'FDResPub')) {
    Set-Service -Name $svc -StartupType Automatic
    if ((Get-Service -Name $svc).Status -ne 'Running') {
        Start-Service -Name $svc
    }
}

# Allow guest SMB logons — the Host share is guest-accessible (no
# credentials). Windows blocks passwordless shares by default.
$lanman = 'HKLM:\SYSTEM\CurrentControlSet\Services\LanmanWorkstation\Parameters'
Set-ItemProperty -Path $lanman -Name AllowInsecureGuestAuth -Value 1 -Type DWord
Write-Host 'AllowInsecureGuestAuth = 1 (guest share access enabled)'

Get-NetFirewallRule |
    Where-Object { $_.DisplayGroup -match 'Discovery|发现|File and Printer Sharing|文件和打印机共享' } |
    Enable-NetFirewallRule

Get-Service -Name fdPHost, FDResPub |
    Select-Object Name, Status, StartType | Format-Table -AutoSize
Get-NetConnectionProfile -InterfaceIndex $nic.InterfaceIndex |
    Select-Object InterfaceAlias, NetworkCategory | Format-Table -AutoSize
