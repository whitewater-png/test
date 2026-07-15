#!/bin/bash
# DesktopMate.app バンドルを生成するスクリプト
#
# 使い方:
#   ./scripts/build-app.sh [バージョン]
#   例) ./scripts/build-app.sh 1.0.0
#
# ・既定は「このMacのアーキテクチャ」でビルドします（確実・高速）。
#   配布用に Apple Silicon + Intel の両対応にするには DESKTOPMATE_UNIVERSAL=1 を付けます
#   （scripts/package.sh はこれを付けて呼びます）。
# ・icon/AppIcon-1024.png があれば .icns を生成してアイコンを埋め込みます。
# ・ad-hoc 署名まで行います（未署名配布向け。手順は DISTRIBUTION.md 参照）。
set -euo pipefail
cd "$(dirname "$0")/.."

APP_NAME="DesktopMate"
VERSION="${1:-1.0.0}"
# 配布する場合は自分のドメインを逆にした値に変えてください（例: com.yourname.desktopmate）
BUNDLE_ID="${DESKTOPMATE_BUNDLE_ID:-com.example.desktopmate}"
APP_DIR="dist/${APP_NAME}.app"

# 既定は単一アーキ。DESKTOPMATE_UNIVERSAL=1 のときだけユニバーサル。
# （macOS 標準の bash 3.2 では set -u と空配列展開が両立しないため、配列は使わない）
if [[ "${DESKTOPMATE_UNIVERSAL:-0}" == "1" ]]; then
  echo "==> ユニバーサルバイナリをビルド (arm64 + x86_64)..."
  swift build -c release --arch arm64 --arch x86_64
  BIN_DIR="$(swift build -c release --arch arm64 --arch x86_64 --show-bin-path | tail -1)"
else
  echo "==> リリースビルド..."
  swift build -c release
  BIN_DIR="$(swift build -c release --show-bin-path | tail -1)"
fi
BIN="${BIN_DIR}/${APP_NAME}"
if [[ ! -x "$BIN" ]]; then
  echo "✗ ビルド成果物が見つかりません: $BIN"
  exit 1
fi

echo "==> .app バンドルを作成..."
rm -rf dist
mkdir -p "${APP_DIR}/Contents/MacOS" "${APP_DIR}/Contents/Resources"
cp "$BIN" "${APP_DIR}/Contents/MacOS/${APP_NAME}"

# リソースバンドル（bundle.js / viewer.html を含む DesktopMate_DesktopMate.bundle）を同梱。
# Bundle.module は .app の Contents/Resources を探すため、ここに置く。
copied_bundle=0
for b in "${BIN_DIR}"/*.bundle; do
  if [[ -e "$b" ]]; then
    cp -R "$b" "${APP_DIR}/Contents/Resources/"
    copied_bundle=1
  fi
done
if [[ "$copied_bundle" == "0" ]]; then
  echo "   ⚠ リソースバンドル(*.bundle)が見つかりませんでした（VRM表示に必要）。"
  echo "     ${BIN_DIR} を確認してください。"
fi

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
echo "✓ 完了: ${APP_DIR}（バージョン ${VERSION}）"
echo "  起動: open \"${APP_DIR}\""
echo "  配布用の .zip / .dmg を作るには: ./scripts/package.sh ${VERSION}"
