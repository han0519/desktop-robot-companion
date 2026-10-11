@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
title 桌面摆件机器人 一键烧录 v1.1.0

echo ================================================
echo   桌面摆件机器人 (ESP32-S3) 一键烧录 v1.1.0
echo   固件文件: robot_companion_merged.bin
echo   要求: 电脑已装 Python 3 (勾选 Add to PATH)
echo ================================================
echo.

set "BIN=%~dp0robot_companion_merged.bin"
if not exist "%BIN%" (
  echo [错误] 找不到 robot_companion_merged.bin
  echo 请把 flash.bat 和 robot_companion_merged.bin 放在同一文件夹。
  pause
  exit /b 1
)

where python >nul 2>&1
if errorlevel 1 (
  echo [错误] 未检测到 Python。请先安装:
  echo   https://www.python.org/downloads/
  echo   安装时务必勾选 "Add python.exe to PATH"
  pause
  exit /b 1
)

python -c "import esptool" >nul 2>&1
if errorlevel 1 (
  echo 正在安装 esptool ...
  python -m pip install --quiet esptool
  if errorlevel 1 (
    echo [错误] esptool 安装失败，请手动运行:  python -m pip install esptool
    pause
    exit /b 1
  )
  echo esptool 安装完成。
)

set "PORT="
if not "%~1"=="" set "PORT=%~1"
if "%PORT%"=="" (
  echo.
  echo 正在自动检测 ESP32-S3 串口 (请先把机器人用数据线连电脑)...
  for /L %%i in (1,1,20) do (
    python -m esptool --chip esp32s3 -p COM%%i --before default_reset chip_id >nul 2>&1
    if !errorlevel! equ 0 (
      set "PORT=COM%%i"
      echo 检测到设备在 COM%%i
      goto :found
    )
  )
)
:found
if "%PORT%"=="" (
  echo 没有自动检测到设备。请输入串口编号:
  set /p "PIN=例如 3 表示 COM3 (直接回车退出): "
  set "PIN=!PIN:COM=!"
  if "!PIN!"=="" exit /b 1
  set "PORT=COM!PIN!"
)

echo.
echo 开始烧录到 %PORT% ...
python -m esptool --chip esp32s3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "%BIN%"
if errorlevel 1 (
  echo.
  echo [失败] 烧录出错。可能原因:
  echo   1. 线不是数据线 / 没插好
  echo   2. 需要按住开发板 BOOT 键再插 USB
  echo   3. 串口被其它软件占用 (先关掉串口监视器)
  pause
  exit /b 1
)

echo.
echo ================================================
echo   [成功] 固件烧录完成，设备正在自动重启...
echo   首次开机开热点 Robot-Config，手机连上后配置 WiFi。
echo   ================================================
pause