@echo off
REM ---------------------------------------------------------------
REM  Oniro / OpenHarmony x86_general QEMU Emulator Launcher
REM  Windows wrapper — delegates to run.sh via Git Bash or MSYS2.
REM ---------------------------------------------------------------

REM Try Git Bash first
where bash >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [INFO] Found bash, launching run.sh...
    bash "%~dp0run.sh" %*
    exit /b %ERRORLEVEL%
)

REM Try common Git Bash install path
if exist "C:\Program Files\Git\bin\bash.exe" (
    echo [INFO] Found Git Bash, launching run.sh...
    "C:\Program Files\Git\bin\bash.exe" "%~dp0run.sh" %*
    exit /b %ERRORLEVEL%
)

echo [ERROR] bash is not available on this system.
echo.
echo This script requires Git Bash or MSYS2 to run.
echo Install one of the following:
echo   Git for Windows : https://gitforwindows.org/
echo   MSYS2           : https://www.msys2.org/
echo.
echo After installation, re-run this script or run directly:
echo   bash run.sh [OPTIONS] [IMAGE_DIR]
exit /b 1
