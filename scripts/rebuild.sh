#!/bin/bash
# クリーン再ビルド + 起動を行い、各段階の成否を明示するスクリプト。
# 「修正したのに同じエラーが出る」ときは、まずこれを実行して出力を共有してください。
set -uo pipefail
cd "$(dirname "$0")/.."

echo "==> 1) 起動中の DesktopMate を終了"
pkill -f DesktopMate 2>/dev/null || true

echo "==> 2) 現在のコミット: $(git rev-parse --short HEAD 2>/dev/null) / $(git log -1 --format=%s 2>/dev/null)"

echo "==> 3) ビルドキャッシュを削除"
swift package clean 2>/dev/null || true
rm -rf .build dist

echo "==> 4) コンパイル (swift build -c release)"
if ! swift build -c release 2>&1 | tee /tmp/desktopmate_build.log; then
  echo ""
  echo "✗ ビルド失敗(コンパイルエラー)。"
  echo "  → /tmp/desktopmate_build.log の内容(特に error: の行)を貼ってください。"
  echo "  これが出ている間は、古いビルドが起動し続けるため同じ症状が消えません。"
  exit 1
fi
echo "✓ コンパイル成功"

echo "==> 5) .app バンドルを作成"
./scripts/build-app.sh

echo "==> 6) 起動"
open dist/DesktopMate.app
echo ""
echo "✓ 完了: コミット $(git rev-parse --short HEAD) の新しいビルドを起動しました。"
echo "  設定 →「モデルを選択…」で VRM/GLB を指定してください。"
