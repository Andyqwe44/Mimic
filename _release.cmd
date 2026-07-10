@echo off
cd /d "%~dp0"
git add -A
git add -f release/GameAgentMonitor/bin/
git commit -m "chore: release v0.3.4

Co-Authored-By: Claude <noreply@anthropic.com>"
git tag -a v0.3.4 -m "v0.3.4"
git push origin v0.3.4
git push origin main
del _release.cmd
del _retag.cmd 2>NUL
del _commit.cmd 2>NUL
echo ALL_DONE
