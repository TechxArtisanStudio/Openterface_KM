# =============================================================================
# Openterface KM Build and Flash Script for Windows
# Builds firmware and flashes it to the device.
#
# Usage:
#   .\build-and-flash.ps1 [options] [target]
#   .\build-and-flash.ps1 kvmgo              # Build and flash KVMGo
# =============================================================================

param(
    [switch]$NoBle,
    [switch]$NoReset,
    [switch]$DryRun,
    [switch]$Help,
    [string]$CompilerPath = "",
    [string]$Firmware = "",
    [string]$FlashTool = "auto",
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

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir

function Show-Help {
    Write-Host "Openterface KM Build and Flash Script for Windows" -ForegroundColor $Colors.Blue
    Write-Host ""
    Write-Host "Usage: .\build-and-flash.ps1 [options] <target>" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Targets:" -ForegroundColor $Colors.White
    Write-Host "  kvmgo          Build and flash KVMGo firmware" -ForegroundColor $Colors.White
    Write-Host "  minikvm        Build and flash Mini-KVM firmware" -ForegroundColor $Colors.White
    Write-Host "  keymod-plus    Build and flash KeyMod Plus firmware" -ForegroundColor $Colors.White
    Write-Host "  keymod-mini    Build and flash KeyMod Mini firmware" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Options:" -ForegroundColor $Colors.White
    Write-Host "  --no-ble                  Disable Bluetooth" -ForegroundColor $Colors.White
    Write-Host "  --no-reset                Don't reset device after flashing" -ForegroundColor $Colors.White
    Write-Host "  --compiler-path PATH      Specify RISC-V compiler path" -ForegroundColor $Colors.White
    Write-Host "  --firmware <path>         Flash custom firmware (skip build)" -ForegroundColor $Colors.White
    Write-Host "  --flash-tool <tool>       Force flash tool (wchisp|wchisp_cmd)" -ForegroundColor $Colors.White
    Write-Host "  --dry-run                 Print commands without executing" -ForegroundColor $Colors.White
    Write-Host "  --help                    Show this help message" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Examples:" -ForegroundColor $Colors.White
    Write-Host "  .\build-and-flash.ps1 kvmgo                 # Build and flash KVMGo" -ForegroundColor $Colors.White
    Write-Host "  .\build-and-flash.ps1 --firmware custom.bin # Flash custom firmware" -ForegroundColor $Colors.White
    Write-Host "  .\build-and-flash.ps1 --no-ble minikvm      # Build without BLE and flash" -ForegroundColor $Colors.White
}

if ($Help) {
    Show-Help
    exit 0
}

if ($Target -eq "" -and $Firmware -eq "") {
    Write-Host "Error: Please specify a target or firmware file" -ForegroundColor $Colors.Red
    Write-Host ""
    Show-Help
    exit 1
}

Write-Host ""
Write-Host "========================================" -ForegroundColor $Colors.Blue
Write-Host "  Openterface KM Build & Flash" -ForegroundColor $Colors.Blue
Write-Host "========================================" -ForegroundColor $Colors.Blue
Write-Host ""

# Step 1: Build (if firmware not specified)
if ($Firmware -eq "") {
    Write-Host "Step 1/2: Building firmware..." -ForegroundColor $Colors.Blue
    Write-Host ""

    $buildScript = Join-Path $RepoRoot "build.ps1"
    $buildArgs = @($Target)

    if ($NoBle) { $buildArgs = @("-NoBle") + $buildArgs }
    if ($CompilerPath -ne "") { $buildArgs = @("-CompilerPath", $CompilerPath) + $buildArgs }

    & $buildScript @buildArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "Build failed! Aborting flash." -ForegroundColor $Colors.Red
        exit 1
    }

    Write-Host ""
    Write-Host "Build completed successfully!" -ForegroundColor $Colors.Green
    Write-Host ""
} else {
    Write-Host "Step 1/2: Skipping build (using custom firmware)" -ForegroundColor $Colors.Yellow
    Write-Host ""
}

# Step 2: Flash
Write-Host "Step 2/2: Flashing firmware..." -ForegroundColor $Colors.Blue
Write-Host ""

$flashScript = Join-Path $RepoRoot "tools\flash.ps1"
$flashArgs = @()

if ($Target -ne "") { $flashArgs += $Target }
if ($Firmware -ne "") { $flashArgs += @("--firmware", $Firmware) }
if ($NoReset) { $flashArgs += "--no-reset" }
if ($DryRun) { $flashArgs += "--dry-run" }
if ($FlashTool -ne "auto") { $flashArgs += @("--tool", $FlashTool) }

& $flashScript @flashArgs
$flashResult = $LASTEXITCODE

Write-Host ""
if ($flashResult -eq 0) {
    Write-Host "========================================" -ForegroundColor $Colors.Green
    Write-Host "  Build & Flash Complete!" -ForegroundColor $Colors.Green
    Write-Host "========================================" -ForegroundColor $Colors.Green
} else {
    Write-Host "========================================" -ForegroundColor $Colors.Red
    Write-Host "  Flash Failed!" -ForegroundColor $Colors.Red
    Write-Host "========================================" -ForegroundColor $Colors.Red
}

exit $flashResult
