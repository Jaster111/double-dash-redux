@echo off
rem Launch Mario Kart: Double Dash!! (native Windows build).  Build first with tools\run.ps1.
cd /d "%~dp0"
if not exist "out\Release\doubledash.exe" (
  echo out\Release\doubledash.exe not found - build it first:  powershell -ExecutionPolicy Bypass -File tools\run.ps1 -Seconds 1
  pause
  exit /b 1
)
start "" "out\Release\doubledash.exe"
