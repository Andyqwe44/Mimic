#!/bin/bash
# release.sh — one-shot release entry (git-bash friendly).
#
# Usage:  after bumping monitor_app/src/version.h (APP_VERSION + APP_VERSION_RC),
#         run a single command:
#
#             bash release.sh 0.3.5
#
# It chains the whole verified pipeline so a version bump is the ONLY manual edit:
#   1. build_release.cmd  — compile all libs+app, assemble release\, isolated
#                           verify (poll for "prod: frontend served"), git
#                           commit+tag+push.  Run inside a fresh cmd with a clean
#                           minimal PATH + NoDefaultCurrentDirectoryInExePath
#                           cleared, so git-bash's environment can't break it.
#   2. publish_release.sh — create the Gitee Release + upload the installer.
#   3. verify raw URL     — GET raw/<tag>/.../version.json, expect 200 + version.
#
# Existing Gitee releases for OTHER tags are untouched; a same-tag release is
# rebuilt by publish_release.sh. build_release.cmd force-updates the git tag.
set -e

VER="${1:?Usage: bash release.sh <version>   e.g.  bash release.sh 0.3.5}"
cd "$(dirname "$0")"

# --- 1. Sanity: version.h must already declare this version (铁律 8: single source) ---
VH="monitor_app/src/version.h"
if ! grep -q "define APP_VERSION \"$VER\"" "$VH"; then
  echo "ERROR: $VH does not declare  APP_VERSION \"$VER\"."
  echo "       Edit it first (APP_VERSION + APP_VERSION_RC ${VER//./,},0), then re-run."
  exit 1
fi

# --- 2. build_release.cmd in a clean cmd environment ---
# Clean minimal PATH: repeated vcvars appends can't overflow cmd's 8191 PATH cap.
# (build_release.cmd also clears NoDefaultCurrentDirectoryInExePath itself.)
echo "=== [1/3] build_release.cmd $VER (isolated auto-verify) ==="
BAT="${TMPDIR:-/tmp}/gam_release_$$.bat"
{
  echo '@echo off'
  echo 'set "NoDefaultCurrentDirectoryInExePath="'
  echo 'set "PATH=C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0;C:\Program Files\nodejs;C:\Program Files\Git\cmd"'
  echo "cd /d $(cygpath -w "$PWD")"
  echo 'set VERIFY_MODE=--auto'
  echo "call .\\build_release.cmd $VER"
} | sed 's/$/\r/' > "$BAT"
MSYS_NO_PATHCONV=1 cmd /c "$(cygpath -w "$BAT")"
rc=$?
rm -f "$BAT"
if [ $rc -ne 0 ]; then
  echo "build_release failed (rc=$rc) — isolated verify did not pass; nothing published."
  exit 1
fi

# --- 3. Publish to Gitee ---
echo "=== [2/3] publish_release.sh $VER ==="
bash publish_release.sh "$VER"

# --- 4. Verify raw version.json URL (302 -> 200 + version match) ---
echo "=== [3/3] verify raw version.json ==="
U="https://gitee.com/Andyqwe44/tictactoe/raw/v$VER/release/GameAgentMonitor/version.json"
TMP="${TMPDIR:-/tmp}/gam_relverify_$$.json"
code=$(curl -sL -o "$TMP" -w '%{http_code}' "$U")
app=$(grep -o '"app":[^,]*' "$TMP" | head -1)
rm -f "$TMP"
echo "  $U"
echo "  -> HTTP $code ($app)"
if [ "$code" != "200" ]; then
  echo "WARNING: raw URL did not return 200 — check the Gitee push."
  exit 1
fi

echo ""
echo "=========================================="
echo "  release $VER DONE"
echo "=========================================="
echo "  Installer download:"
echo "  https://gitee.com/Andyqwe44/tictactoe/releases/download/v$VER/GameAgentMonitor_Setup_v$VER.exe"
