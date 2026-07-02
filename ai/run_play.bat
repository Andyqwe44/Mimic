@echo off
cd /d %~dp0
echo =========================================
echo   TicTacToe - Human vs AI
echo =========================================
echo.
echo [1/2] Starting AI play server in new window...
start "AI-Play" cmd /k "cd /d %~dp0 && python ai_server.py --model model.pkl --port 9999"
echo.
echo [2/2] Waiting for server to be ready...
timeout /t 2 /nobreak >nul
echo Starting game...
echo.
"%~dp0..\game\main.exe" --server 127.0.0.1 9999 --ai X
echo.
echo =========================================
echo   Game over.
echo =========================================
