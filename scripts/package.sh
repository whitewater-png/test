#!/bin/bash
# 配布用に DesktopMate.app を .zip と .dmg にまとめるスクリプト。
#
# 使い方:
#   ./scripts/package.sh [バージョン]
#   例) ./scripts/package.sh 1.0.0
#
# 先に build-app.sh を呼んで .app を作り直してから梱包します。
set -euo pipefail
cd "$(dirname "$0")/.."

APP_NAME="DesktopMate"
VERSION="${1:-1.0.0}"
APP_DIR="dist/${APP_NAME}.app"

# 最新の .app をビルド
./scripts/build-app.sh "$VERSION"

echo "==> .zip を作成..."
ZIP="dist/${APP_NAME}-${VERSION}.zip"
# ditto を使うと macOS のバンドル構造・属性を保ったまま zip 化できる
ditto -c -k --sequesterRsrc --keepParent "$APP_DIR" "$ZIP"
echo "   $ZIP"

echo "==> .dmg を作成..."
DMG="dist/${APP_NAME}-${VERSION}.dmg"
STAGE="$(mktemp -d)/${APP_NAME}"
mkdir -p "$STAGE"
cp -R "$APP_DIR" "$STAGE/"
# ドラッグ&ドロップでインストールできるよう /Applications への symlink を置く
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
echo "   $DMG"

echo ""
echo "✓ 配布物ができました:"
echo "   - $ZIP"
echo "   - $DMG"
echo "  GitHub Releases などにアップロードして配布できます。"
echo "  受け取った人の開き方は DISTRIBUTION.md を同梱/案内してください。"
