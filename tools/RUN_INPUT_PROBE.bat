@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
    set PY=py
) else (
    where python >nul 2>nul
    if errorlevel 1 (
        echo Python was not found. Install Python 3 and try again.
        pause
        exit /b 1
    )
    set PY=python
)

echo.
set /p THREE_DS_IP=3DS IP [192.168.0.28]: 
if "%THREE_DS_IP%"=="" set THREE_DS_IP=192.168.0.28

%PY% pokebot_input_probe_v0p1.py --ip %THREE_DS_IP%

echo.
pause
