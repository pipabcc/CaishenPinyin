@echo off
setlocal
title Caishen IME Portable - Install

cd /d "%~dp0"

echo ========================================================
echo   [Caishen IME Portable] Installer
echo ========================================================
echo.

echo Checking administrator privileges...
fltmc >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Administrator privileges are required!
    echo [INFO] Please right-click this file and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

if not exist "%~dp0ShuruIme.dll" (
    echo.
    echo [ERROR] ShuruIme.dll not found! Please extract the full zip archive before running.
    echo.
    pause
    exit /b 1
)

echo [1/2] Registering input method COM/TSF component...
regsvr32.exe /s "%~dp0ShuruIme.dll"
if %errorlevel% neq 0 (
    echo [ERROR] regsvr32 failed with error code %errorlevel%
    echo.
    pause
    exit /b 1
)
echo [OK] COM/TSF component registered successfully.

echo [2/2] Configuring system language profile and shortcuts...
if not exist "%~dp0register_user.ps1" (
    echo [ERROR] register_user.ps1 not found. Please extract the full zip archive.
    regsvr32.exe /u /s "%~dp0ShuruIme.dll"
    pause
    exit /b 2
)
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0register_user.ps1"
if errorlevel 1 (
    echo [ERROR] User language profile or shortcut configuration failed.
    echo [INFO] Rolling back COM/TSF registration...
    regsvr32.exe /u /s "%~dp0ShuruIme.dll"
    pause
    exit /b 2
)
echo [OK] System environment configured successfully.

echo.
echo ========================================================
echo   [SUCCESS] Caishen IME Portable installed successfully!
echo   Please press Win + Space to switch to Caishen IME.
echo ========================================================
echo.
pause
exit /b 0
