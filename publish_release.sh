#!/bin/bash
# publish_release.sh — Gitee release management
# Usage: bash publish_release.sh <version> [--force]
#   version: "0.3.4" etc (no 'v' prefix)
#   --force: skip confirmation prompt

set -e
VER="${1:?Usage: bash publish_release.sh <version>}"
shift

# === Config ===
TOKEN="26b26b041e3a6ac124ed8dc7d7c71e84"
REPO="Andyqwe44/tictactoe"
API="https://gitee.com/api/v5/repos/$REPO"
TAG="v$VER"
INSTALLER="release/GameAgentMonitor_Setup_v$VER.exe"

if [ ! -f "$INSTALLER" ]; then
  echo "ERROR: $INSTALLER not found. Run build_release.cmd first."
  exit 1
fi

echo "=== Publish $TAG to Gitee ==="
echo "Installer: $INSTALLER ($(du -h "$INSTALLER" | cut -f1))"
echo ""

# === Find existing release for this tag ===
EXISTING_ID=$(curl -s "$API/releases" | grep -oE "\"id\":[0-9]+,\"tag_name\":\"$TAG\"" | grep -oE '[0-9]+' | head -1)

if [ -n "$EXISTING_ID" ]; then
  echo "Found existing release id=$EXISTING_ID"
  echo "Deleting old release..."
  curl -s -o /dev/null -w "  HTTP %{http_code}\n" -X DELETE "$API/releases/$EXISTING_ID?access_token=$TOKEN"
fi

# === Create new release ===
echo "Creating release $TAG ..."
RELEASE=$(curl -s -X POST "$API/releases?access_token=$TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"tag_name\":\"$TAG\",\"name\":\"$TAG\",\"body\":\"$TAG release\",\"prerelease\":false,\"target_commitish\":\"main\"}")

RELEASE_ID=$(echo "$RELEASE" | grep -oE '"id":[0-9]+' | head -1 | grep -oE '[0-9]+')

if [ -z "$RELEASE_ID" ]; then
  echo "FAILED to create release:"
  echo "$RELEASE" | head -3
  exit 1
fi
echo "  Release ID: $RELEASE_ID"

# === Upload installer ===
echo "Uploading installer..."
UPLOAD=$(curl -s -w "\n  HTTP %{http_code}" -X POST \
  "$API/releases/$RELEASE_ID/attach_files?access_token=$TOKEN" \
  -F "file=@$INSTALLER")
echo "$UPLOAD"

DOWNLOAD_URL=$(echo "$UPLOAD" | grep -oE '"browser_download_url":"[^"]*"' | head -1 | cut -d'"' -f4)

echo ""
echo "=========================================="
echo "  $TAG published!"
echo "=========================================="
echo "  Download: $DOWNLOAD_URL"
echo ""
