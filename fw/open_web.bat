@echo off
rem ============================================================
rem  SBB UDS Flash Console  --  double-click to open
rem
rem  Design:
rem    * the server runs in THIS window (visible, Ctrl+C to stop)
rem    * a 3 s one-shot opens the default browser
rem    * we test `import can` on each python candidate, because
rem      several "python" exist and only one has python-can
rem    * if the server fails we show the traceback instead of
rem      opening a dead tab
rem ============================================================
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set PORT=8080
set URL=http://127.0.0.1:%PORT%/

rem ---------- already running? ----------
call :probe
if "!UP!"=="1" (
  echo.
  echo   Server already running on port %PORT%.
  echo   Opening browser only.
  echo.
  explorer %URL%
  exit /b 0
)

rem ---------- find a python that really has python-can ----------
set PY=
for %%P in (
  "C:\Users\zh244\AppData\Local\Python\pythoncore-3.14-64\python.exe"
  "python3"
  "python"
) do (
  if "!PY!"=="" (
    %%~P -c "import can" >nul 2>&1
    if !errorlevel! equ 0 set PY=%%~P
  )
)

if "!PY!"=="" (
  echo.
  echo   [ERROR] No Python with python-can found.
  echo   Tried: pythoncore-3.14-64, python3, python
  echo   Fix  : pip install python-can
  echo.
  pause
  exit /b 1
)

echo ============================================================
echo   SBB UDS Flash Console
echo   Python : !PY!
echo   URL    : %URL%
echo.
echo   Keep this window open while you use the page.
echo   Press Ctrl+C here to stop the server.
echo ============================================================
echo.

rem ---------- open the browser shortly after the server binds ----------
start "" cmd /c "ping -n 4 127.0.0.1 >nul & explorer %URL%"

rem ---------- run the server in this window ----------
"!PY!" -u tools\uds_web.py

echo.
echo   Server stopped.
pause
exit /b 0

rem ============================================================
:probe
set UP=0
for /f "tokens=*" %%L in ('netstat -ano ^| findstr /R /C:":%PORT% " ^| findstr /C:"LISTENING"') do set UP=1
exit /b 0
