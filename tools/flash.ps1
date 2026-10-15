# =============================================================================
# Openterface KM Firmware Flash Script for Windows
# Flashes firmware to CH32V20x device via USB ISP bootloader.
#
# Usage:
#   .\flash.ps1 [options] [target]
#   .\flash.ps1 kvmgo              # Flash KVMGo firmware
#   .\flash.ps1 --firmware custom.bin
# =============================================================================

param(
    [switch]$NoReset,
    [switch]$DryRun,
    [switch]$Help,
    [string]$Tool = "auto",        # auto | wchisp | wchisp_cmd
    [string]$Firmware = "",
    [Parameter(Position=0)]
    [ValidateSet('kvmgo', 'minikvm', 'keymod-plus', 'keymod-mini', '')]
    [string]$Target = ""
)

$ErrorActionPreference = 'Stop'

# Color definitions
$Colors = @{
    Red = 'Red'
    Green = 'Green'
    Yellow = 'Yellow'
    Blue = 'Blue'
    White = 'White'
}

# Project directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

# Tool paths (customize these if needed)
$WchispPath = "$env:USERPROFILE\.cargo\bin\wchisp.exe"
$WchispToolCmdPath = "$env:USERPROFILE\project\WCHISPTool_CMD\Windows\bin\WCHISPTool_CMD.exe"
$WchispStudioPath = "D:\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Others\WCHISPTool\default\WCHISPTool_CH32Vxxx\WCHISPTool_CH32Vxxx.exe"
$WchispConfig = Join-Path $RepoRoot "tools\flash_clean.ini"

# Firmware file to use
$FirmwareFile = ""

function Show-Help {
    Write-Host "Openterface KM Flash Tool for Windows" -ForegroundColor $Colors.Blue
    Write-Host ""
    Write-Host "Usage: .\flash.ps1 [options] <target>" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Targets:" -ForegroundColor $Colors.White
    Write-Host "  kvmgo          Flash KVMGo firmware (BOARD_VERSION=1)" -ForegroundColor $Colors.White
    Write-Host "  minikvm        Flash Mini-KVM firmware (BOARD_VERSION=2)" -ForegroundColor $Colors.White
    Write-Host "  keymod-plus    Flash KeyMod Plus firmware" -ForegroundColor $Colors.White
    Write-Host "  keymod-mini    Flash KeyMod Mini firmware" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Options:" -ForegroundColor $Colors.White
    Write-Host "  --tool <wchisp|wchisp_cmd>  Force specific flash tool (default: auto)" -ForegroundColor $Colors.White
    Write-Host "  --firmware <path>           Custom firmware file (.bin or .hex)" -ForegroundColor $Colors.White
    Write-Host "  --no-reset                  Don't reset device after flashing" -ForegroundColor $Colors.White
    Write-Host "  --dry-run                   Print commands without executing" -ForegroundColor $Colors.White
    Write-Host "  --help                      Show this help message" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Examples:" -ForegroundColor $Colors.White
    Write-Host "  .\flash.ps1 kvmgo                          # Flash KVMGo firmware" -ForegroundColor $Colors.White
    Write-Host "  .\flash.ps1 --tool wchisp kvmgo            # Force use wchisp" -ForegroundColor $Colors.White
    Write-Host "  .\flash.ps1 --firmware custom.bin          # Flash custom firmware" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Device must be in BOOT/ISP mode:" -ForegroundColor $Colors.Yellow
    Write-Host "  - USB PID 0x55E0, Vendor 0x4348" -ForegroundColor $Colors.Yellow
    Write-Host "  - Hold BOOT button while plugging in USB" -ForegroundColor $Colors.Yellow
}

function Log($msg) {
    Write-Host "[flash] $msg" -ForegroundColor $Colors.Blue
}

function Ok($msg) {
    Write-Host "[flash] $msg" -ForegroundColor $Colors.Green
}

function Warn($msg) {
    Write-Host "[flash] $msg" -ForegroundColor $Colors.Yellow
}

function Err($msg) {
    Write-Host "[flash] $msg" -ForegroundColor $Colors.Red
}

function Run-Command {
    param([string]$Command, [string[]]$Arguments)

    if ($DryRun) {
        Write-Host "+ $Command $($Arguments -join ' ')"
        return 0
    }

    $process = Start-Process -FilePath $Command -ArgumentList $Arguments -NoNewWindow -Wait -PassThru
    return $process.ExitCode
}

# Resolve firmware file path
function Resolve-Firmware {
    if ($Firmware -ne "") {
        if (-not (Test-Path $Firmware)) {
            Err "Firmware file not found: $Firmware"
            exit 1
        }
        $script:FirmwareFile = $Firmware
        return
    }

    if ($Target -eq "") {
        Err "No target specified. Use: .\flash.ps1 kvmgo|minikvm|keymod-plus|keymod-mini"
        Err "   Or: .\flash.ps1 --firmware <path>"
        exit 1
    }

    # Search for firmware in output/ and build/ directories
    $searchDirs = @("output", "build")
    $found = $false

    foreach ($dir in $searchDirs) {
        $binFile = Join-Path $RepoRoot "$dir\openterface-km-firmware-$Target.bin"
        $hexFile = Join-Path $RepoRoot "$dir\openterface-km-firmware-$Target.hex"

        if (Test-Path $binFile) {
            $script:FirmwareFile = $binFile
            $found = $true
            break
        }
        if (Test-Path $hexFile) {
            $script:FirmwareFile = $hexFile
            $found = $true
            break
        }
    }

    if (-not $found) {
        Err "Firmware not found for target: $Target"
        Err "Build first: .\build.ps1 $Target"
        Err "Searched in: output\ and build\"
        exit 1
    }
}

# Detect available flash tool
function Detect-Tool {
    $wchispOk = (Test-Path $WchispPath) -and (Get-Command $WchispPath -ErrorAction SilentlyContinue)
    $cmdOk = (Test-Path $WchispToolCmdPath) -and (Get-Command $WchispToolCmdPath -ErrorAction SilentlyContinue)
    $studioOk = Test-Path $WchispStudioPath

    # Also check PATH
    if (-not $wchispOk) {
        $wchispOk = Get-Command wchisp -ErrorAction SilentlyContinue
        if ($wchispOk) { $script:WchispPath = "wchisp" }
    }
    if (-not $cmdOk) {
        $cmdOk = Get-Command WCHISPTool_CMD -ErrorAction SilentlyContinue
        if ($cmdOk) { $script:WchispToolCmdPath = "WCHISPTool_CMD" }
    }

    switch ($Tool) {
        "wchisp" {
            if (-not $wchispOk) {
                Err "wchisp not found at $WchispPath"
                exit 1
            }
            return "wchisp"
        }
        "wchisp_cmd" {
            if (-not $cmdOk) {
                Err "WCHISPTool_CMD not found at $WchispToolCmdPath"
                exit 1
            }
            return "wchisp_cmd"
        }
        "studio" {
            if (-not $studioOk) {
                Err "WCHISPTool Studio not found at $WchispStudioPath"
                exit 1
            }
            return "studio"
        }
        "auto" {
            if ($wchispOk) {
                return "wchisp"
            } elseif ($cmdOk) {
                return "wchisp_cmd"
            } elseif ($studioOk) {
                return "studio"
            } else {
                Err "No flash tool found."
                Err "Install wchisp: cargo install wchisp --git https://github.com/ch32-rs/wchisp"
                Err "Or download WCHISPTool_CMD from WCH official site."
                exit 1
            }
        }
    }
}

# Wait for device in BOOT mode
function Wait-ForDevice {
    $maxWait = 15
    $waited = 0

    Log "Waiting for device in BOOT mode (max ${maxWait}s)..."

    while ($waited -lt $maxWait) {
        # Check for WCH ISP device (PID 0x55E0)
        $devices = Get-PnpDevice -Class USB -ErrorAction SilentlyContinue |
                   Where-Object { $_.InstanceId -match "VID_4348&PID_55E0|VID_1A86&PID_55E0" }

        if ($devices) {
            Ok "Device detected!"
            return $true
        }

        Start-Sleep -Seconds 1
        $waited++
        Write-Host "." -NoNewline
    }

    Write-Host ""
    Err "Device not detected after ${maxWait}s."
    Err ""
    Err "Troubleshooting:"
    Err "  1. Ensure device is plugged in via USB"
    Err "  2. Enter BOOT mode: hold BOOT button while plugging in"
    Err "  3. Check Device Manager for USB PID 0x55E0, Vendor 0x4348"
    Err "  4. Try a different USB port or cable"
    Err ""
    Err "Check Windows Device Manager:"
    Err "  Get-PnpDevice -Class USB | Where-Object { `$_.InstanceId -match '55E0' }"

    return $false
}

# Flash with wchisp
function Flash-Wchisp {
    param([string]$FwFile)

    Log "Flashing with wchisp: $FwFile"

    $exitCode = Run-Command -Command $WchispPath -Arguments @("flash", $FwFile)

    if ($exitCode -eq 0) {
        Ok "Flash successful!"
        return $true
    } else {
        Err "Flash failed with wchisp (exit code: $exitCode)"
        return $false
    }
}

# Flash with WCHISPTool_CMD
function Flash-WchispCmd {
    param([string]$FwFile)

    Log "Flashing with WCHISPTool_CMD: $FwFile"

    # Remove code protection first
    Log "Removing code protection..."

    $args = @("-c", $WchispConfig, "-o", "program", "-r", "-f", $FwFile)
    $exitCode = Run-Command -Command $WchispToolCmdPath -Arguments $args

    if ($exitCode -eq 0) {
        Ok "Flash successful!"
        return $true
    } elseif ($exitCode -eq 6) {
        Err "Device not found (Code 6). Possible causes:"
        Err "  - Device not in BOOT mode"
        Err "  - Chip type mismatch in config: check $WchispConfig"
        return $false
    } elseif ($exitCode -eq 12) {
        Err "Download failed (Code 12). Try removing code protection first:"
        Err "  $WchispToolCmdPath -c $WchispConfig -r"
        return $false
    } else {
        Err "Flash failed with code $exitCode"
        return $false
    }
}

# Flash with WCHISPTool Studio (GUI)
function Flash-Studio {
    param([string]$FwFile)

    Log "Opening WCHISPTool Studio for flashing..."
    Log "Please use the GUI to flash: $FwFile"
    Log "Steps:"
    Log "  1. Device must be in BOOT mode (hold BOOT button while plugging in)"
    Log "  2. Select chip type: CH32V20x"
    Log "  3. Click 'Open File' and select: $FwFile"
    Log "  4. Click 'Download' to flash"

    Start-Process -FilePath $WchispStudioPath -ArgumentList $FwFile
    Ok "WCHISPTool Studio launched. Please follow the GUI instructions."
    return $true
}

# Main script
if ($Help) {
    Show-Help
    exit 0
}

Write-Host ""
Write-Host "========================================" -ForegroundColor $Colors.Blue
Write-Host "  Openterface KM Flash Tool (Windows)" -ForegroundColor $Colors.Blue
Write-Host "========================================" -ForegroundColor $Colors.Blue
Write-Host ""

Resolve-Firmware
$flashTool = Detect-Tool

Log "Target:      $(if ($Target) { $Target } else { 'custom' })"
Log "Firmware:    $FirmwareFile"
Log "Flash tool:  $flashTool"
Log "Reset after: $(-not $NoReset)"
Write-Host ""

# Check device
if (-not (Wait-ForDevice)) {
    exit 1
}

# Flash
$success = $false
switch ($flashTool) {
    "wchisp" { $success = Flash-Wchisp -FwFile $FirmwareFile }
    "wchisp_cmd" { $success = Flash-WchispCmd -FwFile $FirmwareFile }
    "studio" { $success = Flash-Studio -FwFile $FirmwareFile }
}

if ($success) {
    Write-Host ""
    Ok "============================================"
    Ok "  Flash complete!"
    Ok "============================================"
    Write-Host ""
    Log "Device should now enumerate as a USB HID device."
    Log "Check: Get-PnpDevice -Class USB | Where-Object { `$_.InstanceId -match 'Openterface' }"
    exit 0
} else {
    exit 1
}
