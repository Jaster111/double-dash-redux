@echo off
rem Launch Double Dash Redux.  Build first with tools\build.ps1.
cd /d "%~dp0"
if not exist "out\Release\DoubleDashRedux.exe" (
  echo out\Release\DoubleDashRedux.exe not found - build it first:  powershell -ExecutionPolicy Bypass -File tools\build.ps1
  pause
  exit /b 1
)
start "" "out\Release\DoubleDashRedux.exe"
