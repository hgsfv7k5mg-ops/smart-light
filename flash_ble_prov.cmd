@echo off
REM ============================================================
REM  BLE provisioning demo: erase flash first, then flash.
REM
REM  WHY ERASE:
REM  The WiFi driver stores the last credentials in the NVS
REM  partition (WIFI_STORAGE_FLASH). Without erasing it the board
REM  would reboot straight into your home WiFi and NEVER start the
REM  BLE advertisement - so the phone app would find nothing.
REM
REM  PRE-REQUISITE:
REM    Build the project first, e.g.:
REM      powershell -ExecutionPolicy Bypass -File build.ps1 -ExtraArgs build
REM
REM  AFTER FLASHING:
REM    1) phone app: ESP BLE Prov (ESP-IDF Provisioning)
REM    2) scan -> pick device named  PROV_LIGHT_xxxx
REM    3) POP / proof of possession: abcd1234
REM    4) choose your WiFi, type the password, send
REM    5) board reboots and connects with the new credentials
REM ============================================================

set PORT=COM3

echo.
echo ============================================
echo   Erase flash + flash + monitor   on %PORT%
echo ============================================
echo.

powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" -ExtraArgs "-p %PORT% erase-flash flash monitor"

echo.
echo [DONE] monitor closed.
pause
