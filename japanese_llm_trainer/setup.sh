#!/bin/bash
# =============================================================================
# セットアップスクリプト
# MacBook Pro M4 Max 向け 日本語LLMファインチューニング環境構築
#
# 使い方:
#   chmod +x setup.sh
#   ./setup.sh
# =============================================================================

set -euo pipefail

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

log_step() { echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}\n${GREEN}[Step $1] $2${NC}\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"; }
log_info()  { echo -e "${YELLOW}▶ $1${NC}"; }
log_done()  { echo -e "${GREEN}✓ $1${NC}"; }
log_error() { echo -e "${RED}✗ エラー: $1${NC}"; exit 1; }
log_warn()  { echo -e "${CYAN}⚠ $1${NC}"; }

echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║  日本語LLM ファインチューニング 環境セットアップ     ║"
echo "║  MacBook Pro M4 Max 36GB 向け                        ║"
echo "╚══════════════════════════════════════════════════════╝"
echo ""

# =============================================================================
# Step 1: システム要件チェック
# =============================================================================
log_step 1 "システム要件チェック"

# macOS確認
OS=$(uname)
if [ "$OS" != "Darwin" ]; then
  log_error "このスクリプトはmacOS専用です（現在: $OS）"
fi
MACOS_VER=$(sw_vers -productVersion)
log_done "macOS $MACOS_VER"

# Apple Silicon確認
CHIP=$(uname -m)
if [ "$CHIP" != "arm64" ]; then
  log_error "Apple Silicon（M1/M2/M3/M4）が必要です（現在: $CHIP）"
fi
CHIP_NAME=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo "Apple Silicon")
log_done "チップ: $CHIP_NAME"

# メモリ確認
MEM_GB=$(sysctl -n hw.memsize | awk '{printf "%d", $1/1024/1024/1024}')
echo -e "  メモリ: ${MEM_GB}GB"
if [ "$MEM_GB" -lt 16 ]; then
  log_error "最低16GBのメモリが必要です（現在: ${MEM_GB}GB）"
elif [ "$MEM_GB" -lt 36 ]; then
  log_warn "推奨は36GB、現在${MEM_GB}GBです。14Bモデルは厳しい場合があります。8Bモデルを検討してください。"
else
  log_done "メモリ: ${MEM_GB}GB（最適）"
fi

# Python確認
if ! command -v python3 &>/dev/null; then
  log_error "Python3が見つかりません。https://www.python.org からインストールしてください。"
fi
PY_VER=$(python3 --version 2>&1 | awk '{print $2}')
PY_MAJOR=$(echo "$PY_VER" | cut -d. -f1)
PY_MINOR=$(echo "$PY_VER" | cut -d. -f2)
if [ "$PY_MAJOR" -lt 3 ] || ([ "$PY_MAJOR" -eq 3 ] && [ "$PY_MINOR" -lt 11 ]); then
  log_warn "Python 3.11以上を推奨します（現在: $PY_VER）"
  log_warn "動作する可能性はありますが、問題が起きたらpyenvで3.11をインストールしてください"
else
  log_done "Python $PY_VER"
fi

# =============================================================================
# Step 2: Pythonライブラリのインストール
# =============================================================================
log_step 2 "Pythonライブラリのインストール"
log_info "mlx-lm, datasets, huggingface_hub などをインストールします..."

pip3 install -r requirements.txt --quiet

# mlx-lmの動作確認
python3 -c "import mlx_lm; print('  mlx-lm バージョン:', mlx_lm.__version__)" || \
  log_error "mlx-lmのインストールに失敗しました"
log_done "ライブラリのインストール完了"

# =============================================================================
# Step 3: HuggingFace ログイン
# =============================================================================
log_step 3 "HuggingFace ログイン"

# すでにログイン済みか確認
if huggingface-cli whoami &>/dev/null; then
  HF_USER=$(huggingface-cli whoami 2>/dev/null | head -1)
  log_done "ログイン済み: $HF_USER"
else
  log_info "HuggingFaceへのログインが必要です。"
  log_info "アカウントをお持ちでない場合: https://huggingface.co/join で登録（無料）"
  echo ""
  huggingface-cli login
  if ! huggingface-cli whoami &>/dev/null; then
    log_error "HuggingFaceへのログインに失敗しました"
  fi
  log_done "ログイン成功"
fi

# =============================================================================
# Step 4: モデルの選択とダウンロード
# =============================================================================
log_step 4 "モデルのダウンロード"

echo ""
echo "  使用するモデルを選んでください:"
echo ""
echo "  1) Qwen3-14B（推奨）"
echo "     - 品質高め、約28GBダウンロード"
echo "     - 学習時メモリ: ~21GB → 他アプリに15GB残る"
echo ""
echo "  2) Qwen3-8B（軽量版）"
echo "     - やや品質低め、約15GBダウンロード"
echo "     - 学習時メモリ: ~12GB → 他アプリに24GB残る"
echo ""
read -p "  番号を入力 [1/2] (デフォルト: 1): " MODEL_CHOICE
MODEL_CHOICE=${MODEL_CHOICE:-1}

if [ "$MODEL_CHOICE" = "2" ]; then
  HF_MODEL="Qwen/Qwen3-8B"
  MODEL_DIR="./models/qwen3-8b"
  MODEL_SIZE="8B"
  DOWNLOAD_SIZE="~15GB"
  PIPELINE_FLAG="--model 8b"
else
  HF_MODEL="Qwen/Qwen3-14B"
  MODEL_DIR="./models/qwen3-14b"
  MODEL_SIZE="14B"
  DOWNLOAD_SIZE="~28GB"
  PIPELINE_FLAG="--model 14b"
fi

if [ -d "$MODEL_DIR" ] && [ "$(ls -A $MODEL_DIR 2>/dev/null)" ]; then
  log_done "モデルはすでにダウンロード済みです: $MODEL_DIR"
else
  log_info "モデルをダウンロードします: $HF_MODEL（${DOWNLOAD_SIZE}）"
  log_info "Wi-Fiで実行してください。時間は回線速度により異なります（目安: 光回線で30〜60分）"
  echo ""
  mkdir -p "$MODEL_DIR"
  huggingface-cli download "$HF_MODEL" \
    --local-dir "$MODEL_DIR" \
    --exclude "*.pt" \
    --repo-type model
  log_done "モデルダウンロード完了: $MODEL_DIR"
fi

# =============================================================================
# Step 5: 動作確認（クイックテスト）
# =============================================================================
log_step 5 "動作確認"
log_info "モデルを読み込んで日本語で一言生成します（約1〜2分）..."

python3 - <<PYEOF
import subprocess, sys
result = subprocess.run(
    ["python3", "-m", "mlx_lm.generate",
     "--model", "$MODEL_DIR",
     "--prompt", "こんにちは！自己紹介をお願いします。",
     "--max-tokens", "80",
     "--temp", "0.7"],
    capture_output=True, text=True
)
if result.returncode == 0:
    print("\n  [生成結果]")
    for line in result.stdout.split("\n"):
        if line.strip() and not line.startswith("="):
            print("  " + line)
    print()
else:
    print("  エラー:", result.stderr[-300:])
    sys.exit(1)
PYEOF

log_done "動作確認OK！モデルが正常に動作しています"

# =============================================================================
# 完了
# =============================================================================
echo ""
echo "╔══════════════════════════════════════════════════════╗"
echo "║            セットアップ完了！                        ║"
echo "╠══════════════════════════════════════════════════════╣"
echo "║  次のステップ:                                       ║"
echo "║                                                      ║"
echo "║  1) まず動作テスト（約15〜20分）:                    ║"
printf "║     ./run_pipeline.sh %-32s║\n" "$PIPELINE_FLAG --all --test"
echo "║                                                      ║"
echo "║  2) 本番実行（就寝前に開始・約12〜15時間）:          ║"
printf "║     ./run_pipeline.sh %-32s║\n" "$PIPELINE_FLAG --all"
echo "║                                                      ║"
echo "║  ※ スリープ防止コマンドと組み合わせると安心:         ║"
printf "║     caffeinate -i ./run_pipeline.sh %-16s║\n" "$PIPELINE_FLAG --all"
echo "╚══════════════════════════════════════════════════════╝"
echo ""
