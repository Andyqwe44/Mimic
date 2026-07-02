@echo off
cd /d %~dp0
echo =========================================
echo   TicTacToe AI Training
echo =========================================
echo.
echo [1/2] Starting AI training server in new window...
start "AI-Train" cmd /k "cd /d %~dp0 && python train.py --iters 50 --games 100"
echo.
echo [2/2] Waiting for server to be ready...
timeout /t 3 /nobreak >nul
echo Starting self-play games...
echo.
"%~dp0..\game\main.exe" --server 127.0.0.1 9999 --auto --games 5000
echo.
echo =========================================
echo   Training session complete.
echo =========================================
