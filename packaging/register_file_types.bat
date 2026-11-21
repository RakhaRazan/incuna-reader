@echo off
setlocal enabledelayedexpansion

REM ============================================================================
REM  Incuna Reader Optional Windows Explorer File Association Script (Per-User)
REM  - Never run automatically during portable usage.
REM  - Registers .cbz, .cbr, .cb7, .cbt, .pdf, and .epub under HKCU\Software\Classes
REM    (no Administrator privileges required).
REM  - Pass /unregister (or --unregister) to remove all associations.
REM ============================================================================

set "ACTION=register"
if /i "%~1"=="/unregister" set "ACTION=unregister"
if /i "%~1"=="--unregister" set "ACTION=unregister"
if /i "%~1"=="/u" set "ACTION=unregister"

set "SCRIPT_DIR=%~dp0"
set "EXE_PATH=%SCRIPT_DIR%IncunaReader.exe"
if not exist "%EXE_PATH%" set "EXE_PATH=%SCRIPT_DIR%..\IncunaReader.exe"
if not "%~1"=="" if /i not "%ACTION%"=="unregister" if exist "%~1" set "EXE_PATH=%~1"

for %%I in ("%EXE_PATH%") do set "EXE_PATH=%%~fI"

if /i "%ACTION%"=="unregister" (
    echo Removing Incuna Reader per-user file associations...
    reg delete "HKCU\Software\Classes\IncunaReader.Comic" /f >nul 2>&1
    for %%E in (.cbz .cbr .cb7 .cbt .pdf .epub) do (
        reg delete "HKCU\Software\Classes\%%E\OpenWithProgids" /v "IncunaReader.Comic" /f >nul 2>&1
    )
    reg delete "HKCU\Software\Classes\Applications\IncunaReader.exe" /f >nul 2>&1
    echo Done. Incuna Reader file associations removed.
    exit /b 0
)

if not exist "%EXE_PATH%" (
    echo ERROR: IncunaReader.exe not found at "%EXE_PATH%".
    echo Place this script next to IncunaReader.exe or run build_portable.bat first.
    exit /b 1
)

echo Registering Incuna Reader file associations for current user...
echo Executable: %EXE_PATH%
echo.

REM Define IncunaReader.Comic ProgID
reg add "HKCU\Software\Classes\IncunaReader.Comic" /ve /d "Incuna Reader Comic Book" /f >nul
reg add "HKCU\Software\Classes\IncunaReader.Comic" /v "AppUserModelID" /d "IncunaReader.Portable.App" /f >nul
reg add "HKCU\Software\Classes\IncunaReader.Comic\DefaultIcon" /ve /d "\"%EXE_PATH%\",0" /f >nul
reg add "HKCU\Software\Classes\IncunaReader.Comic\shell\open\command" /ve /d "\"%EXE_PATH%\" \"%%1\"" /f >nul

REM Register in Applications\IncunaReader.exe for Windows "Open with" menu
reg add "HKCU\Software\Classes\Applications\IncunaReader.exe\shell\open\command" /ve /d "\"%EXE_PATH%\" \"%%1\"" /f >nul
reg add "HKCU\Software\Classes\Applications\IncunaReader.exe" /v "FriendlyAppName" /d "Incuna Reader" /f >nul

REM Associate .cbz, .cbr, .cb7, .cbt, .pdf, .epub via OpenWithProgids
for %%E in (.cbz .cbr .cb7 .cbt .pdf .epub) do (
    reg add "HKCU\Software\Classes\%%E\OpenWithProgids" /v "IncunaReader.Comic" /t REG_NONE /f >nul
    reg add "HKCU\Software\Classes\Applications\IncunaReader.exe\SupportedTypes" /v "%%E" /d "" /f >nul
)

REM Notify Windows Explorer of association changes
powershell -NoProfile -Command "Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class Sh { [DllImport(\"shell32.dll\")] public static extern void SHChangeNotify(int wEventId, int uFlags, IntPtr dwItem1, IntPtr dwItem2); }'; [Sh]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)" >nul 2>&1

echo.
echo SUCCESS: Registered .cbz, .cbr, .cb7, .cbt, .pdf, and .epub for Incuna Reader.
echo To undo later, run: %~nx0 --unregister
endlocal
exit /b 0
