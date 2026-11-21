@echo off
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fI"
set "FRONTEND_DIR=%ROOT_DIR%\frontend"
set "BACKEND_DIR=%ROOT_DIR%\backend-cpp"
set "BUILD_DIR=%BACKEND_DIR%\build"
set "MASTER_DIR=%ROOT_DIR%"

echo ============================================================
echo  Incuna Reader Portable Master Builder (x64 Release)
echo ============================================================
echo Master Folder: %MASTER_DIR%
echo.

REM 1. Build frontend/dist
echo [1/4] Building frontend (npm run build)...
pushd "%FRONTEND_DIR%"
if not exist "node_modules" (
    call npm ci
    if errorlevel 1 (
        popd
        echo ERROR: npm ci failed.
        exit /b 1
    )
)
call npm run build
if errorlevel 1 (
    popd
    echo ERROR: frontend build failed.
    exit /b 1
)
popd

REM 2. Configure & compile backend-cpp in Release x64 mode
echo.
echo [2/4] Building backend-cpp (Release x64)...
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
    for %%C in (
        "C:\Program Files\CMake\bin\cmake.exe"
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    ) do (
        if exist %%C set "CMAKE_EXE=%%~C"
    )
)
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    "%CMAKE_EXE%" -S "%BACKEND_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 (
        echo ERROR: CMake configure failed.
        exit /b 1
    )
)
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release --target IncunaReader --parallel
if errorlevel 1 (
    echo ERROR: CMake Release build failed.
    exit /b 1
)

REM 3. Deploy canonical executable & runtime directly into master folder
echo.
echo [3/4] Deploying to master folder: %MASTER_DIR%
if exist "%ROOT_DIR%\release" rmdir /s /q "%ROOT_DIR%\release"
if exist "%MASTER_DIR%\KomaReader.exe" del /f /q "%MASTER_DIR%\KomaReader.exe"
if exist "%MASTER_DIR%\runtime\dist" rmdir /s /q "%MASTER_DIR%\runtime\dist"
if not exist "%MASTER_DIR%\runtime" mkdir "%MASTER_DIR%\runtime"
mkdir "%MASTER_DIR%\runtime\dist"
if not exist "%MASTER_DIR%\data" mkdir "%MASTER_DIR%\data"

copy /y "%BUILD_DIR%\Release\IncunaReader.exe" "%MASTER_DIR%\IncunaReader.exe" >nul
if errorlevel 1 (
    echo ERROR: Failed to copy IncunaReader.exe.
    exit /b 1
)

if exist "%BUILD_DIR%\Release\pdfium.dll" (
    copy /y "%BUILD_DIR%\Release\pdfium.dll" "%MASTER_DIR%\runtime\pdfium.dll" >nul
) else (
    echo ERROR: pdfium.dll not found in %BUILD_DIR%\Release
    exit /b 1
)

REM Copy any optional runtime DLLs if present (WebView2Loader / libarchive are statically linked)
if exist "%BUILD_DIR%\Release\WebView2Loader.dll" (
    copy /y "%BUILD_DIR%\Release\WebView2Loader.dll" "%MASTER_DIR%\runtime\WebView2Loader.dll" >nul
)
for %%D in ("%BUILD_DIR%\Release\archive*.dll") do (
    if exist "%%~fD" copy /y "%%~fD" "%MASTER_DIR%\runtime\" >nul
)

xcopy "%FRONTEND_DIR%\dist\*" "%MASTER_DIR%\runtime\dist\" /E /I /Y /Q >nul
if errorlevel 1 (
    echo ERROR: Failed to copy frontend/dist to runtime/dist.
    exit /b 1
)

REM 4. Verify master folder deployment
echo.
echo [4/4] Verifying master folder deployment...
powershell -NoProfile -Command ^
  "$root = '%MASTER_DIR%';" ^
  "if (-not (Test-Path (Join-Path $root 'IncunaReader.exe'))) { Write-Error 'Missing IncunaReader.exe'; exit 2 };" ^
  "if (-not (Test-Path (Join-Path $root 'runtime\pdfium.dll'))) { Write-Error 'Missing runtime\pdfium.dll'; exit 3 };" ^
  "if (-not (Test-Path (Join-Path $root 'runtime\dist\index.html'))) { Write-Error 'Missing runtime\dist\index.html'; exit 4 };" ^
  "if (-not (Test-Path (Join-Path $root 'data'))) { Write-Error 'Missing data/'; exit 5 };" ^
  "if (Test-Path (Join-Path $root 'release')) { Write-Error 'Redundant release/ folder still exists'; exit 6 };" ^
  "$exeSize = [math]::Round((Get-Item (Join-Path $root 'IncunaReader.exe')).Length / 1MB, 2);" ^
  "Write-Host ('  Canonical EXE: ' + (Join-Path $root 'IncunaReader.exe') + ' (' + $exeSize + ' MB)');" ^
  "Write-Host ('  Runtime dir:   ' + (Join-Path $root 'runtime'));" ^
  "Write-Host ('  Data dir:      ' + (Join-Path $root 'data'))"

if errorlevel 1 (
    echo ERROR: Master folder deployment verification failed.
    exit /b 1
)

echo.
echo SUCCESS: IncunaReader.exe deployed to %MASTER_DIR%\IncunaReader.exe
endlocal
exit /b 0
