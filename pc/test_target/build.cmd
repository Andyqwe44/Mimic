@echo off
cd /d "%~dp0"
powershell -NoProfile -File "%~dp0..\scripts\Build.ps1" -Module test_target
exit /b %ERRORLEVEL%
