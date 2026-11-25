# Verify that the Fedora Host is discoverable and its share is reachable.
# Run from the Windows target:
#   powershell -ExecutionPolicy Bypass -File tools\verify_windows_discovery.ps1
$ErrorActionPreference = 'Continue'

Write-Host '--- net view \\FEDORA ---'
net view '\\FEDORA'

Write-Host '--- share listing ---'
Get-ChildItem '\\FEDORA\SharedWithTarget' |
    Select-Object Name, Length | Format-Table -AutoSize

Write-Host '--- SMB connections ---'
Get-SmbConnection | Select-Object ServerName, ShareName, Dialect | Format-Table -AutoSize

Write-Host '--- enabled discovery/firewall rules ---'
$rules = Get-NetFirewallRule | Where-Object {
    ($_.DisplayGroup -match '发现|Discovery|文件和打印机共享|File and Printer Sharing') -and
    ($_.Enabled -eq 'True')
}
Write-Host "Enabled rules: $($rules.Count)"

Write-Host '--- NCM adapter profile ---'
$nic = Get-NetIPAddress -AddressFamily IPv4 -IPAddress '192.168.20.2' -ErrorAction SilentlyContinue
if ($nic) {
    Get-NetConnectionProfile -InterfaceIndex $nic.InterfaceIndex |
        Select-Object InterfaceAlias, NetworkCategory | Format-Table -AutoSize
} else {
    Write-Warning 'NCM IP 192.168.20.2 not found'
}
