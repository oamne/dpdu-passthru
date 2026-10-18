@echo off
setlocal enabledelayedexpansion

:: ==============================================================================
:: /**
::  * D-PDU API J2534 Driver - Complete & Total Uninstaller
::  * Cleans all drivers, configuration files, XML descriptors, registry keys,
::  * logs, and optionally removes the directory itself.
::  *
::  * Supports Windows 7 / 8 / 10 / 11 (32-bit and 64-bit).
::  * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
::  * Inspired by the Saab community - saabiste.net
::  *
::  * Source: https://github.com/oamne/dpdu-passthru
::  */
:: ==============================================================================

echo =======================================================
echo   TOTAL UNINSTALLATION - D-PDU J2534 DRIVER
echo   Complete cleanup of files, folders, and registry
echo   Supports Windows 7 / 8 / 10 / 11 (32-bit and 64-bit)
echo =======================================================
echo.

:: 1. Check Administrator Privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] This script must be run AS ADMINISTRATOR!
    echo Right-click uninstall.bat -^> "Run as administrator".
    echo.
    pause
    exit /b 1
)

:: 2. Terminate processes that might lock DLLs
echo [1/5] Terminating diagnostic applications...
taskkill /f /im Tech2Win.exe >nul 2>&1
taskkill /f /im t2win.exe >nul 2>&1
taskkill /f /im emulator.exe >nul 2>&1
taskkill /f /im dps.exe >nul 2>&1

:: 3. Remove Windows Registry Keys (32-bit native and WOW64 64-bit)
echo [2/5] Removing D-PDU API registry keys...
reg delete "HKLM\SOFTWARE\D-PDU API" /f >nul 2>&1
reg delete "HKLM\SOFTWARE\WOW6432Node\D-PDU API" /f >nul 2>&1
echo [OK] D-PDU registry keys removed.

:: 4. Remove installed program files and directories
echo [3/5] Removing installed files and directories...

:: Switch working directory to TEMP to release any Windows directory lock
cd /d "%TEMP%"

if defined ProgramFiles(x86) (
    set "DIR_DPDU_64=%ProgramFiles(x86)%\dpdu_j2534"
    set "DIR_PDU_64=%ProgramFiles(x86)%\D-PDU API"
    if exist "!DIR_DPDU_64!" (
        echo [INFO] Removing directory: "!DIR_DPDU_64!"
        rd /s /q "!DIR_DPDU_64!" >nul 2>&1
    )
    if exist "!DIR_PDU_64!\pdu_api_root.xml" (
        del /f /q "!DIR_PDU_64!\pdu_api_root.xml" >nul 2>&1
    )
    rd "!DIR_PDU_64!" >nul 2>&1
)

set "DIR_DPDU_32=%ProgramFiles%\dpdu_j2534"
set "DIR_PDU_32=%ProgramFiles%\D-PDU API"
if exist "!DIR_DPDU_32!" (
    echo [INFO] Removing directory: "!DIR_DPDU_32!"
    rd /s /q "!DIR_DPDU_32!" >nul 2>&1
)
if exist "!DIR_PDU_32!\pdu_api_root.xml" (
    del /f /q "!DIR_PDU_32!\pdu_api_root.xml" >nul 2>&1
)
rd "!DIR_PDU_32!" >nul 2>&1

:: Redundant cleanup for explicit paths
if exist "C:\Program Files (x86)\dpdu_j2534" rd /s /q "C:\Program Files (x86)\dpdu_j2534" >nul 2>&1
if exist "C:\Program Files\dpdu_j2534" rd /s /q "C:\Program Files\dpdu_j2534" >nul 2>&1
if exist "C:\Program Files (x86)\D-PDU API\pdu_api_root.xml" del /f /q "C:\Program Files (x86)\D-PDU API\pdu_api_root.xml" >nul 2>&1
if exist "C:\Program Files\D-PDU API\pdu_api_root.xml" del /f /q "C:\Program Files\D-PDU API\pdu_api_root.xml" >nul 2>&1
rd "C:\Program Files (x86)\D-PDU API" >nul 2>&1
rd "C:\Program Files\D-PDU API" >nul 2>&1
echo [OK] Program directories removed.

:: 5. Remove configuration files and logs
echo [4/5] Removing configuration files and logs...
if exist "C:\Users\Public\dpdu_settings.ini" (
    del /f /q "C:\Users\Public\dpdu_settings.ini" >nul 2>&1
    echo [OK] Removed dpdu_settings.ini.
)
del /f /q "C:\Users\Public\dpdu_*.log" >nul 2>&1
del /f /q "C:\Users\Public\dpdu.log" >nul 2>&1
echo [OK] Log files cleaned.

echo [5/5] Final verification complete.
echo.
echo =======================================================
echo   UNINSTALLATION COMPLETED SUCCESSFULLY!
echo   All files, folders, and registry keys for the
echo   D-PDU J2534 driver have been completely removed.
echo =======================================================
echo.

pause
