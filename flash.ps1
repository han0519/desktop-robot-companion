# Flash desktop-robot-companion (ESP32-S3) over USB-UART.
#
# Usage:
#   .\flash.ps1                 # flash bootloader + partition table + app (COM5)
#   .\flash.ps1 -Port COM9      # different port
#   .\flash.ps1 -Erase          # erase the whole flash first (wipes NVS/creds)
param(
    [string]$Port = 'COM5',
    [switch]$Erase
)

$ErrorActionPreference = 'Continue'

$idfPath = 'Z:\ESPIDF\Espressif\frameworks\esp-idf-v5.5.3'
$pyEnv   = 'Z:\ESPIDF\Espressif\python_env\idf5.5_py3.11_env'
$proj    = 'H:\esp32-s3-work\projects\desktop-robot-companion'

$env:IDF_PATH              = $idfPath
$env:IDF_TOOLS_PATH        = 'Z:\ESPIDF\Espressif'
$env:IDF_PYTHON_ENV_PATH   = $pyEnv
$env:IDF_COMPONENT_MANAGER = '1'
$env:ESP_IDF_VERSION      = '5.5.3'
$env:PYTHONUTF8            = '1'
$env:IDF_MAINTAINER        = '1'
$env:IDF_TARGET            = 'esp32s3'

$cmakeBin = 'Z:\ESPIDF\Espressif\tools\cmake\3.30.2\bin'
$ninjaBin = 'Z:\ESPIDF\Espressif\tools\ninja\1.12.1'
$toolBin  = 'Z:\ESPIDF\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin'
if (-not (Test-Path (Join-Path $toolBin 'xtensa-esp-elf-gcc.exe'))) {
    foreach ($d in Get-ChildItem 'Z:\ESPIDF\Espressif\tools\xtensa-esp-elf\*\xtensa-esp-elf\bin' -ErrorAction SilentlyContinue) {
        if (Test-Path (Join-Path $d.FullName 'xtensa-esp-elf-gcc.exe')) { $toolBin = $d.FullName; break }
    }
}
$env:PATH = (@($toolBin, $cmakeBin, $ninjaBin, "$pyEnv\Scripts") + @($env:PATH)) -join ';'

Set-Location $proj
$idfPy = "$pyEnv\Scripts\python.exe"

if ($Erase) {
    Write-Host "===== erasing flash on $Port ====="
    & $idfPy "$idfPath\tools\idf.py" -p $Port erase-flash
    Write-Host "erase exit: $LASTEXITCODE"
}

Write-Host "===== flashing $Port ====="
& $idfPy "$idfPath\tools\idf.py" -p $Port flash
Write-Host "flash exit: $LASTEXITCODE"