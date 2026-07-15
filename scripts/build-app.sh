#!/bin/bash
# DesktopMate.app バンドルを生成するスクリプト（配布用）
#
# 使い方:
#   ./scripts/build-app.sh [バージョン]
#   例) ./scripts/build-app.sh 1.0.0
#
# ・Apple Silicon と Intel の両対応（ユニバーサルバイナリ）でビルドします
# ・icon/AppIcon-1024.png があれば .icns を生成してアイコンを埋め込みます
# ・ad-hoc 署名まで行います（未署名配布向け。手順は DISTRIBUTION.md 参照）
set -euo pipefail
cd "$(dirname "$0")/.."

APP_NAME="DesktopMate"
VERSION="${1:-1.0.0}"
# 配布する場合は自分のドメインを逆にした値に変えてください（例: com.yourname.desktopmate）
BUNDLE_ID="${DESKTOPMATE_BUNDLE_ID:-com.example.desktopmate}"
APP_DIR="dist/${APP_NAME}.app"

echo "==> ユニバーサルバイナリをビルド (arm64 + x86_64)..."
swift build -c release --arch arm64 --arch x86_64
BIN_DIR="$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path)"
BIN="${BIN_DIR}/${APP_NAME}"

echo "==> .app バンドルを作成..."
rm -rf dist
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Resources"
cp "${BIN}" "${APP_DIR}/Contents/MacOS/${APP_NAME}"

# SwiftPM のリソースバンドル（bundle.js / viewer.html など）を一緒に入れる
for b in "${BIN_DIR}"/*.bundle; do
  [ -e "$b" ] && cp -R "$b" "${APP_DIR}/Contents/Resources/"
done

# --- アプリアイコン (.icns) を生成 ---
ICON_SRC="icon/AppIcon-1024.png"
ICON_PLIST=""
if [[ -f "$ICON_SRC" ]] && command -v iconutil >/dev/null 2>&1 && command -v sips >/dev/null 2>&1; then
  echo "==> アプリアイコンを生成..."
  ICONSET="$(mktemp -d)/AppIcon.iconset"
  mkdir -p "$ICONSET"
  for sz in 16 32 128 256 512; do
    sips -z "$sz" "$sz" "$ICON_SRC" --out "$ICONSET/icon_${sz}x${sz}.png" >/dev/null
    sips -z "$((sz*2))" "$((sz*2))" "$ICON_SRC" --out "$ICONSET/icon_${sz}x${sz}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "${APP_DIR}/Contents/Resources/AppIcon.icns"
  ICON_PLIST="    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
"
else
  echo "   (アイコン生成はスキップ: icon/AppIcon-1024.png / iconutil / sips のいずれかが無い)"
fi

echo "==> Info.plist を作成..."
cat > "${APP_DIR}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleDisplayName</key>
    <string>${APP_NAME}</string>
    <key>CFBundleIdentifier</key>
    <string>${BUNDLE_ID}</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleExecutable</key>
    <string>${APP_NAME}</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
${ICON_PLIST}    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSHumanReadableCopyright</key>
    <string>Licensed under the MIT License.</string>
</dict>
</plist>
PLIST

echo "==> ad-hoc 署名..."
codesign --force --deep --sign - "${APP_DIR}"

echo ""
echo "✓ 完了: ${APP_DIR}（バージョン ${VERSION} / ユニバーサル）"
echo "  起動: open \"${APP_DIR}\""
echo "  配布用の .zip / .dmg を作るには: ./scripts/package.sh ${VERSION}"
echo "  ※ ad-hoc 署名のため、配布時はユーザーが初回「右クリック→開く」で許可します（DISTRIBUTION.md 参照）"
