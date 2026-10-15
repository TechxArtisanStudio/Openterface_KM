# =============================================================================
# Openterface KM Firmware Build Script for Windows
# Usage: .\build.ps1 [options] [product version]
# =============================================================================

param(
    [switch]$NoBle,
    [switch]$Help,
    [string]$CompilerPath = "",
    [Parameter(Position=0)]
    [ValidateSet('kvmgo', 'minikvm', 'keymod-plus', 'keymod-mini', 'all')]
    [string]$Target = ""
)

$ErrorActionPreference = 'Stop'

# Color definitions (for Write-Host)
$Colors = @{
    Red = 'Red'
    Green = 'Green'
    Yellow = 'Yellow'
    Blue = 'Blue'
    White = 'White'
}

# Project directory
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ScriptDir "build"
$OutputDir = Join-Path $ScriptDir "output"

# Default parameters
$ENABLE_BLE = 1  # Enabled by default
$USE_LTO = 1     # LTO enabled for all boards
$ENABLE_OSCILLATOR = ""
$BOARD_VERSION = ""
$BLE_NAME = ""

# Mount River Studio toolchain path (Windows default)
$MounRiverToolchainPath = "D:\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin"

# Help information
function Show-Help {
    Write-Host "Openterface KM Firmware Build Script for Windows" -ForegroundColor $Colors.Blue
    Write-Host ""
    Write-Host "Usage: .\build.ps1 [options] <product version>" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Product versions:" -ForegroundColor $Colors.White
    Write-Host "  kvmgo          Build KVMGo (BOARD_VERSION=1)" -ForegroundColor $Colors.White
    Write-Host "  minikvm        Build Mini-KVM v1.2 (BOARD_VERSION=2)" -ForegroundColor $Colors.White
    Write-Host "  keymod-plus    Build KeyMod Plus (external 32KHz oscillator)" -ForegroundColor $Colors.White
    Write-Host "  keymod-mini    Build KeyMod Mini (internal RC oscillator)" -ForegroundColor $Colors.White
    Write-Host "  all            Build all versions" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Options:" -ForegroundColor $Colors.White
    Write-Host "  -NoBle              Disable Bluetooth (saves ~14KB RAM)" -ForegroundColor $Colors.White
    Write-Host "  -CompilerPath PATH  Specify RISC-V compiler path" -ForegroundColor $Colors.White
    Write-Host "  -Help               Show this help message" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Examples:" -ForegroundColor $Colors.White
    Write-Host "  .\build.ps1 kvmgo                    # Build KVMGo firmware" -ForegroundColor $Colors.White
    Write-Host "  .\build.ps1 keymod-plus              # Build KeyMod Plus firmware" -ForegroundColor $Colors.White
    Write-Host "  .\build.ps1 -NoBle kvmgo             # Build KVMGo without Bluetooth" -ForegroundColor $Colors.White
    Write-Host "  .\build.ps1 all                      # Build all versions" -ForegroundColor $Colors.White
    Write-Host ""
    Write-Host "Output directory: $OutputDir\" -ForegroundColor $Colors.White
}

# Check compiler
function Check-Compiler {
    $compilerBinPath = ""

    # If compiler path is specified, use it
    if ($CompilerPath -ne "") {
        $compilerBinPath = $CompilerPath
        Write-Host "Using specified compiler path: $CompilerPath" -ForegroundColor $Colors.Blue
    }
    # Otherwise, try Mount River Studio default path
    elseif (Test-Path $MounRiverToolchainPath) {
        $compilerBinPath = $MounRiverToolchainPath
        Write-Host "Using Mount River Studio toolchain: $MounRiverToolchainPath" -ForegroundColor $Colors.Blue
    }
    # Try to find in PATH
    elseif (Get-Command riscv-none-embed-gcc -ErrorAction SilentlyContinue) {
        Write-Host "Using compiler from PATH" -ForegroundColor $Colors.Blue
        return $true
    }
    else {
        Write-Host "Error: RISC-V compiler not found" -ForegroundColor $Colors.Red
        Write-Host ""
        Write-Host "Please install the RISC-V toolchain:" -ForegroundColor $Colors.White
        Write-Host "  1. Install Mount River Studio from: https://www.mounriver.com/" -ForegroundColor $Colors.White
        Write-Host "  2. Or download: https://github.com/openwch/risc-none-embed-gcc/releases/tag/8.2.0-3.1.1" -ForegroundColor $Colors.White
        Write-Host ""
        Write-Host "Use one of the following methods:" -ForegroundColor $Colors.White
        Write-Host "  a) .\build.ps1 -CompilerPath 'D:\MounRiver_Studio2\...\bin' kvmgo" -ForegroundColor $Colors.White
        Write-Host "  b) Set environment variable: RISC_COMPILER_PATH=D:\...\bin" -ForegroundColor $Colors.White
        return $false
    }

    # Add compiler to PATH
    $env:PATH = "$compilerBinPath;$env:PATH"

    # Verify compiler
    try {
        $compilerPath = Get-Command riscv-none-embed-gcc -ErrorAction Stop
        $compilerVersion = & riscv-none-embed-gcc --version | Select-Object -First 1

        Write-Host "✓ Compiler: $($compilerPath.Source)" -ForegroundColor $Colors.Green
        Write-Host "  Version: $compilerVersion" -ForegroundColor $Colors.Green

        # Warning: check compiler version
        if ($compilerVersion -match "15\.") {
            Write-Host "⚠ Warning: Compiler version 15.x may be incompatible with WCH chips" -ForegroundColor $Colors.Yellow
            Write-Host "  Recommended version 8.2.0" -ForegroundColor $Colors.Yellow
        }

        return $true
    }
    catch {
        Write-Host "Error: Failed to verify compiler" -ForegroundColor $Colors.Red
        return $false
    }
}

# Create output directory
function New-OutputDir {
    if (-not (Test-Path $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    }
}

# Build firmware
function Build-Firmware {
    param(
        [string]$TargetName,
        [string]$BoardVersion,
        [string]$EnableOscillator,
        [string]$BleName
    )

    Write-Host ""
    Write-Host "========================================" -ForegroundColor $Colors.Blue
    Write-Host "Building: $TargetName" -ForegroundColor $Colors.Blue
    Write-Host "========================================" -ForegroundColor $Colors.Blue
    Write-Host "  BOARD_VERSION:     $BoardVersion" -ForegroundColor $Colors.White
    Write-Host "  ENABLE_BLE:        $ENABLE_BLE" -ForegroundColor $Colors.White
    Write-Host "  ENABLE_OSCILLATOR: $EnableOscillator" -ForegroundColor $Colors.White
    Write-Host "  BLE_NAME:          $BleName" -ForegroundColor $Colors.White
    Write-Host ""

    # Set environment variables for the build
    $env:BOARD_VERSION = $BoardVersion
    $env:ENABLE_BLE = $ENABLE_BLE
    $env:BLE_NAME = $BleName
    $env:USE_LTO = $USE_LTO

    if ($EnableOscillator -ne "") {
        $env:ENABLE_OSCILLATOR = $EnableOscillator
    }

    Push-Location $BuildDir

    try {
        # Find bash.exe (prefer Git Bash over WSL)
        $bashPath = $null
        $gitBashPaths = @(
            "E:\Git\usr\bin\bash.exe",
            "C:\Program Files\Git\usr\bin\bash.exe",
            "D:\Program Files\Git\usr\bin\bash.exe",
            "$env:ProgramFiles\Git\usr\bin\bash.exe",
            "$env:ProgramFiles(x86)\Git\usr\bin\bash.exe"
        )
        foreach ($path in $gitBashPaths) {
            if (Test-Path $path) {
                $bashPath = $path
                break
            }
        }
        if (-not $bashPath) {
            # Fallback to bash in PATH (might be WSL)
            $bashCommand = Get-Command bash -ErrorAction SilentlyContinue
            if ($bashCommand) {
                $bashPath = $bashCommand.Source
            } else {
                Write-Host "Error: bash.exe not found. Please install Git for Windows." -ForegroundColor $Colors.Red
                return $false
            }
        }

        # Clean
        Write-Host "Cleaning previous build..." -ForegroundColor $Colors.Yellow
        & $bashPath -c "make clean" 2>&1 | Out-Null

        # Build make arguments
        $makeArgs = "BOARD_VERSION=$BoardVersion ENABLE_BLE=$ENABLE_BLE BLE_NAME=$BleName USE_LTO=$USE_LTO"
        if ($EnableOscillator -ne "") {
            $makeArgs += " ENABLE_OSCILLATOR=$EnableOscillator"
        }

        # Compile
        Write-Host "Compiling..." -ForegroundColor $Colors.YELLOW
        $makeCommand = "make -j$env:NUMBER_OF_PROCESSORS $makeArgs all"

        & $bashPath -c "`"$makeCommand`""
        if ($LASTEXITCODE -ne 0) {
            Write-Host "✗ Build failed: $TargetName" -ForegroundColor $Colors.Red
            return $false
        }

        # Generate binary file
        Write-Host "Generating binary files..." -ForegroundColor $Colors.YELLOW
        & riscv-none-embed-objcopy -O binary "openterface-km-firmware.elf" "openterface-km-firmware.bin"

        # Get firmware size
        $sizeInfo = & riscv-none-embed-size --format=berkeley "openterface-km-firmware.elf"

        # Copy to output directory
        $outputName = "openterface-km-firmware-$TargetName"
        Copy-Item "openterface-km-firmware.hex" "$OutputDir\$outputName.hex" -Force
        Copy-Item "openterface-km-firmware.bin" "$OutputDir\$outputName.bin" -Force
        Copy-Item "openterface-km-firmware.elf" "$OutputDir\$outputName.elf" -Force
        Copy-Item "openterface-km-firmware.map" "$OutputDir\$outputName.map" -Force

        Write-Host "✓ Build succeeded: $TargetName" -ForegroundColor $Colors.GREEN
        Write-Host "  Output file: $OutputDir\$outputName.bin" -ForegroundColor $Colors.GREEN
        Write-Host ""
        Write-Host "Firmware size:" -ForegroundColor $Colors.WHITE
        Write-Host $sizeInfo
        Write-Host ""

        return $true
    }
    finally {
        Pop-Location
    }
}

# Main script
if ($Help) {
    Show-Help
    exit 0
}

if ($Target -eq "") {
    Write-Host "Error: Please specify a product version to build" -ForegroundColor $Colors.Red
    Write-Host ""
    Show-Help
    exit 1
}

# Start build
Write-Host "Openterface KM Firmware Build" -ForegroundColor $Colors.Blue
Write-Host "========================" -ForegroundColor $Colors.White

if (-not (Check-Compiler)) {
    exit 1
}

New-OutputDir

# Build counter
$BUILD_SUCCESS = 0
$BUILD_FAILED = 0

# Build one function
function Build-One {
    param(
        [string]$TargetName,
        [string]$BoardVersion,
        [string]$EnableOscillator,
        [string]$BleName
    )

    if (Build-Firmware -TargetName $TargetName -BoardVersion $BoardVersion -EnableOscillator $EnableOscillator -BleName $BleName) {
        $script:BUILD_SUCCESS++
    } else {
        $script:BUILD_FAILED++
    }
}

# Build by target
switch ($Target) {
    'kvmgo' {
        $ENABLE_BLE = 1
        $USE_LTO = 1
        Build-One "kvmgo" "1" "" "KVM-GO"
    }
    'minikvm' {
        $ENABLE_BLE = 0
        $USE_LTO = 1
        Build-One "minikvm" "2" "" "Mini-KVM"
    }
    'keymod-plus' {
        $ENABLE_BLE = 1
        $USE_LTO = 1
        Build-One "keymod-plus" "3" "1" "KeyMod"
    }
    'keymod-mini' {
        $ENABLE_BLE = 1
        $USE_LTO = 1
        Build-One "keymod-mini" "3" "0" "KeyMod"
    }
    'all' {
        $ENABLE_BLE = 1
        $USE_LTO = 1
        Build-One "kvmgo" "1" "" "KVM-GO"

        $ENABLE_BLE = 0
        Build-One "minikvm" "2" "" "Mini-KVM"

        $ENABLE_BLE = 1
        Build-One "keymod-plus" "3" "1" "KeyMod"
        Build-One "keymod-mini" "3" "0" "KeyMod"
    }
}

# Summary
Write-Host ""
Write-Host "========================================" -ForegroundColor $Colors.Blue
Write-Host "Build complete" -ForegroundColor $Colors.Blue
Write-Host "========================================" -ForegroundColor $Colors.Blue
Write-Host "  Success: $BUILD_SUCCESS" -ForegroundColor $Colors.Green
Write-Host "  Failed:  $BUILD_FAILED" -ForegroundColor $Colors.Red
Write-Host ""
Write-Host "Output directory: $OutputDir\" -ForegroundColor $Colors.White

# List output files
if (Test-Path $OutputDir) {
    Get-ChildItem "$OutputDir\*.bin" | Format-Table Name, Length -AutoSize
}

if ($BUILD_FAILED -gt 0) {
    exit 1
}
