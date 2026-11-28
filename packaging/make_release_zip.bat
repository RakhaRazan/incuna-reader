@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fI"
set "RELEASE_DIR=%ROOT_DIR%\release"

set "VERSION=%~1"
if "%VERSION%"=="" set "VERSION=1.0.0"
if /i "%VERSION:~0,1%"=="v" set "VERSION=%VERSION:~1%"

set "ZIP_NAME=IncunaReader-v%VERSION%-Windows-x64.zip"
set "ZIP_PATH=%RELEASE_DIR%\%ZIP_NAME%"

echo ============================================================
echo  Incuna Reader Portable Release Packager (v%VERSION%)
echo ============================================================
echo Root Folder: %ROOT_DIR%
echo Output Zip : %ZIP_PATH%
echo.

if not exist "%ROOT_DIR%\IncunaReader.exe" (
    echo ERROR: IncunaReader.exe not found in "%ROOT_DIR%".
    echo Run packaging\build_portable.bat first.
    exit /b 1
)

if not exist "%ROOT_DIR%\runtime" (
    echo ERROR: runtime\ folder not found in "%ROOT_DIR%".
    echo Run packaging\build_portable.bat first.
    exit /b 1
)

if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
if exist "%ZIP_PATH%" del /f /q "%ZIP_PATH%"

pushd "%ROOT_DIR%"
echo Compressing IncunaReader.exe and runtime\ (excluding data\)...
tar -a -cf "%ZIP_PATH%" IncunaReader.exe runtime
if errorlevel 1 (
    popd
    echo ERROR: Failed to create release archive.
    exit /b 1
)
popd

echo.
echo [OK] Created: %ZIP_PATH%
exit /b 0
