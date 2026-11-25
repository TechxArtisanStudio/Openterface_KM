# Create a click-friendly shortcut to the Host share.
# The shortcut name contains only legal characters, so it can be clicked
# directly in Windows Network / Desktop instead of typing the WebDAV UNC.
# Run from the Windows target:
#   powershell -ExecutionPolicy Bypass -File tools\create_host_shortcut.ps1
$ErrorActionPreference = 'Stop'

$target = '\\KEYMOD-HOST\SharedWithTarget'
$shortcutName = 'Host SharedWithTarget'

$shell = New-Object -ComObject WScript.Shell

# Show up in the "Network" view (Network Shortcuts folder).
$netDir = Join-Path $env:APPDATA 'Microsoft\Windows\Network Shortcuts'
New-Item -ItemType Directory -Force -Path $netDir | Out-Null
$netLnk = $shell.CreateShortcut((Join-Path $netDir ($shortcutName + '.lnk')))
$netLnk.TargetPath = $target
$netLnk.Description = 'Open the KeyMod Host shared folder'
$netLnk.Save()

# Also put a shortcut on the Desktop for one-click access.
$desktop = [Environment]::GetFolderPath('Desktop')
$deskLnk = $shell.CreateShortcut((Join-Path $desktop ($shortcutName + '.lnk')))
$deskLnk.TargetPath = $target
$deskLnk.Description = 'Open the KeyMod Host shared folder'
$deskLnk.Save()

Write-Host "Created shortcuts named '$shortcutName' pointing to $target"
Get-Item (Join-Path $netDir ($shortcutName + '.lnk')), (Join-Path $desktop ($shortcutName + '.lnk')) |
    Select-Object FullName, Length | Format-Table -AutoSize
