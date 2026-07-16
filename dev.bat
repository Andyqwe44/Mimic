@echo off
rem dev.bat -- double-click entry: Vite + monitor_app (Dev). Logic in scripts\Dev.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\Dev.ps1"
if errorlevel 1 pause
