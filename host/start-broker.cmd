@echo off
REM ==========================================================================
REM  Smart Light - Local MQTT Broker + Web Dashboard launcher
REM  Requires Node.js 18+ and dependencies installed (npm install).
REM  Double-click this file, or run it from cmd.
REM ==========================================================================
chcp 65001 >nul
setlocal

REM ---- Port 8083 already listening? A broker is running: just open the dashboard ----
REM      (cmd.exe parses .cmd with the OEM codepage, so keep this file ASCII-only.)
netstat -ano | findstr /c:":8083 " | findstr /c:"LISTENING" >nul
if not errorlevel 1 (
    echo.
    echo   [INFO] Port 8083 is already in use - the broker is running.
    echo          Opening the dashboard instead of starting a second one.
    echo.
    start "" "http://localhost:8083"
    exit /b 0
)

set "NODE_EXE="

REM 1) Prefer a system-wide Node.js
for /f "delims=" %%i in ('where node 2^>nul') do (
    if not defined NODE_EXE set "NODE_EXE=%%i"
)

if not defined NODE_EXE (
    echo.
    echo [ERROR] node.exe not found.
    echo         Install Node.js 18+ from https://nodejs.org and retry.
    echo.
    pause
    exit /b 1
)

REM Dependencies come from this directory's node_modules (run "npm install" first).
REM Set IOT_NODE_MODULES to point at another node_modules location if needed.
if not exist "%~dp0node_modules" (
    if not defined IOT_NODE_MODULES (
        echo.
        echo [ERROR] Dependencies not installed.
        echo         Run "npm install" in this directory first.
        echo.
        pause
        exit /b 1
    )
)

echo.
echo   Node      : %NODE_EXE%
echo   MQTT TCP  : 0.0.0.0:1883        ^<- ESP32 connects here
echo.
echo   Dashboard : http://localhost:8083
echo   Firmware  : http://localhost:8083/ota/smart_light.bin
echo.
echo   Press Ctrl+C to stop.
echo.

"%NODE_EXE%" "%~dp0broker.js"

echo.
echo Broker stopped.
pause
