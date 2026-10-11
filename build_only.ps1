$ErrorActionPreference = 'Continue'

# desktop-robot-companion (纸壳桌面摆件机器人) targets ESP32-S3, ESP-IDF 5.5.3.
#
# activate.py --export is broken in this environment (same as the Moji project),
# so the IDF 5.5.3 environment is set by hand:
#   - framework : Z:\ESPIDF\Espressif\frameworks\esp-idf-v5.5.3
#   - python env : Z:\ESPIDF\Espressif\python_env\idf5.5_py3.11_env
#   - toolchain : Z:\ESPIDF\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107
$idfPath = 'Z:\ESPIDF\Espressif\frameworks\esp-idf-v5.5.3'
$pyEnv   = 'Z:\ESPIDF\Espressif\python_env\idf5.5_py3.11_env'
$proj    = 'H:\esp32-s3-work\projects\desktop-robot-companion'
$log     = Join-Path $proj 'build_only.log'

$env:IDF_PATH             = $idfPath
$env:IDF_TOOLS_PATH       = 'Z:\ESPIDF\Espressif'
$env:IDF_PYTHON_ENV_PATH  = $pyEnv
$env:IDF_COMPONENT_MANAGER = '1'
$env:ESP_IDF_VERSION      = '5.5.3'
$env:PYTHONUTF8           = '1'
$env:IDF_MAINTAINER       = '1'
$env:IDF_TARGET           = 'esp32s3'

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

Write-Output "===== [$(Get-Date)] BUILD (esp32s3) =====" | Tee-Object -FilePath $log -Append
& "$pyEnv\Scripts\python.exe" "$idfPath\tools\idf.py" build 2>&1 | Tee-Object -FilePath $log -Append
Write-Output "BUILD EXIT: $LASTEXITCODE" | Tee-Object -FilePath $log -Append