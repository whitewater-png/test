#!/bin/bash
# DesktopMate.app バンドルを生成するスクリプト
# 使い方: ./scripts/build-app.sh
set -euo pipefail

cd "$(dirname "$0")/.."

APP_NAME="DesktopMate"
BUILD_DIR=".build/release"
APP_DIR="dist/${APP_NAME}.app"

echo "==> Building release binary..."
swift build -c release

echo "==> Creating app bundle..."
rm -rf "dist"
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Resources"

cp "${BUILD_DIR}/${APP_NAME}" "${APP_DIR}/Contents/MacOS/${APP_NAME}"

cat > "${APP_DIR}/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>DesktopMate</string>
    <key>CFBundleDisplayName</key>
    <string>DesktopMate</string>
    <key>CFBundleIdentifier</key>
    <string>com.example.DesktopMate</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleExecutable</key>
    <string>DesktopMate</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

echo "==> Ad-hoc code signing..."
codesign --force --deep --sign - "${APP_DIR}"

echo ""
echo "完了! dist/${APP_NAME}.app が生成されました。"
echo "open dist/${APP_NAME}.app で起動できます。"
