@echo off
cd /d "%~dp0"
git add -A
git commit -m "feat: fix auto-update raw URL paths, bump v0.3.3"
echo DONE
