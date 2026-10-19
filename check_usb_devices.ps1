# Check for USB devices
Write-Host "=== USB Devices ===" -ForegroundColor Cyan
Get-PnpDevice -Class USB | Select-Object Status, InstanceId, FriendlyName | Format-Table -AutoSize

Write-Host "`n=== Network Adapters ===" -ForegroundColor Cyan
Get-PnpDevice -Class Net | Select-Object Status, InstanceId, FriendlyName | Format-Table -AutoSize

Write-Host "`n=== Serial/COM Ports ===" -ForegroundColor Cyan
Get-PnpDevice -Class Ports | Select-Object Status, InstanceId, FriendlyName | Format-Table -AutoSize

Write-Host "`n=== Looking for CDC-ECM or USB Ethernet devices ===" -ForegroundColor Yellow
Get-PnpDevice | Where-Object {
    $_.FriendlyName -match "ECM|Ethernet|USB.*Net|RNDIS|CDC" -or
    $_.InstanceId -match "VID_1A86|VID_0525"  # CH341 or common USB gadget VIDs
} | Select-Object Status, Class, InstanceId, FriendlyName | Format-Table -AutoSize
