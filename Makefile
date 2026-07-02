.PHONY: game capture input agent monitor all run train play clean clean-all

all: game capture input agent monitor

game:
	cmd //c "cd /d src\game && build.cmd"

capture:
	cmd //c "cd /d src\capture && build.cmd"

input:
	cmd //c "cd /d src\input && build.cmd"

agent:
	cmd //c "cd /d src\agent && build.cmd"

monitor:
	cmd //c "cd /d src\monitor_slint && build.cmd"

run:
	build\game\main.exe

train: game
	ai\run_train.bat

play: game
	ai\run_play.bat

clean:
	cmd //C "del /Q /F ai\*.pkl 2>NUL & rmdir /S /Q ai\__pycache__ 2>NUL & del /Q /F /S ai\*.pyc 2>NUL & rmdir /S /Q ai\logs 2>NUL & ver >nul"

clean-all: clean
	cmd //C "rmdir /S /Q build 2>NUL & mkdir build"
