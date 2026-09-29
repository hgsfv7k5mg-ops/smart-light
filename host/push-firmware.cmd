@echo off
REM ==========================================================================
REM  Copy the freshly built firmware into host\ota\ so devices can pull it
REM  over HTTP (OTA). Run this after every "idf.py build".
REM ==========================================================================
chcp 65001 >nul
setlocal

set "SRC=%~dp0..\build\smart_light.bin"
set "DST=%~dp0ota\smart_light.bin"

if not exist "%SRC%" (
    echo.
    echo [ERROR] Firmware not found: %SRC%
    echo         Build the project first:  idf.py build
    echo.
    pause
    exit /b 1
)

copy /y "%SRC%" "%DST%" >nul
if errorlevel 1 (
    echo [ERROR] Copy failed.
    pause
    exit /b 1
)

echo.
echo   Firmware published.
echo   %DST%
echo   URL: http://localhost:8083/ota/smart_light.bin
echo.
pause
