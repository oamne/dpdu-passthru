@echo off
setlocal enabledelayedexpansion

pushd %~dp0

echo =======================================================
echo   D-PDU API J2534 PassThru Driver - Universal Installer
echo   Supports Windows 7 / 10 / 11 (32-bit and 64-bit)
echo   Supports Saab 9-3 NG (9440) for Tech2Win
echo =======================================================
echo.

:: 0. Check Administrator Privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [!] ERROR: This installer requires ADMINISTRATOR privileges.
    echo [*] Please right-click "install.bat" and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

:: 1. Architecture Detection
if defined ProgramFiles(x86) (
    set "TARGET_PROG=%ProgramFiles(x86)%"
    set "IS_X64=1"
    echo [*] Detected 64-bit Windows OS.
) else (
    set "TARGET_PROG=%ProgramFiles%"
    set "IS_X64=0"
    echo [*] Detected 32-bit Windows OS.
)

set "DPDU_DIR=%TARGET_PROG%\dpdu_j2534"
set "PDU_API_DIR=%TARGET_PROG%\D-PDU API"

echo [*] Driver install folder: "!DPDU_DIR!"
echo [*] D-PDU API root folder: "!PDU_API_DIR!"

:: 2. Create Target Directories
if not exist "!DPDU_DIR!" mkdir "!DPDU_DIR!"
if not exist "!PDU_API_DIR!" mkdir "!PDU_API_DIR!"

:: 3. Configure Windows Registry (32-bit and 64-bit compatible)
echo [*] Registering D-PDU API Root File in Registry...
reg add "HKLM\SOFTWARE\D-PDU API" /v "Root File" /t REG_SZ /d "!PDU_API_DIR!\pdu_api_root.xml" /f >nul 2>&1
if "!IS_X64!"=="1" (
    reg add "HKLM\SOFTWARE\WOW6432Node\D-PDU API" /v "Root File" /t REG_SZ /d "!PDU_API_DIR!\pdu_api_root.xml" /f >nul 2>&1
)

:: 4. Generate Dynamically Adjusted pdu_api_root.xml
echo [*] Configuring D-PDU root configuration...
> "!PDU_API_DIR!\pdu_api_root.xml" echo ^<?xml version="1.0" encoding="UTF-8"?^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo ^<MVCI_PDU_API_ROOT xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="C:\ProgramData\D-PDU API\xsd.xsd" MVCI_PART2_STANDARD_VERSION="2.2.0"^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo   ^<MVCI_PDU_API^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo     ^<SHORT_NAME^>DPDU_J2534^</SHORT_NAME^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo     ^<DESCRIPTION^>D-PDU J2534 PassThru Driver for Saab 9-3 NG 9440^</DESCRIPTION^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo     ^<SUPPLIER_NAME^>github.com/oamne^</SUPPLIER_NAME^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo     ^<LIBRARY_FILE URI="file://!DPDU_DIR!\driver.dll"/^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo     ^<MODULE_DESCRIPTION_FILE URI="file://!DPDU_DIR!\MDF_DPDU.xml"/^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo   ^</MVCI_PDU_API^>
>> "!PDU_API_DIR!\pdu_api_root.xml" echo ^</MVCI_PDU_API_ROOT^>

:: 5. Copy Driver and Module Description File
echo [*] Deploying driver and module files...
copy /y "driver.dll" "!DPDU_DIR!\driver.dll" >nul
if errorlevel 1 (
    echo [!] ERROR: Failed to copy driver.dll. Please run this script as Administrator.
    pause
    popd
    exit /b 1
)

copy /y "MDF_DPDU.xml" "!DPDU_DIR!\MDF_DPDU.xml" >nul
if exist "uninstall.bat" copy /y "uninstall.bat" "!DPDU_DIR!\uninstall.bat" >nul

:: 6. Deploy Settings File
if not exist "C:\Users\Public" mkdir "C:\Users\Public"
copy /y "dpdu_settings.ini" "C:\Users\Public\dpdu_settings.ini" >nul

echo.
echo =======================================================
echo   Installation SUCCESSFUL!
echo   Tech2Win can now access DPDU_J2534 hardware.
echo =======================================================
echo.
pause
popd