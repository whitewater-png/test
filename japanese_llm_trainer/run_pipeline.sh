#!/bin/bash
# =============================================================================
# Qwen3-8B 日本語ファインチューニング フルパイプライン
# MacBook Pro M4 Max 36GB 向け
#
# 使い方:
#   chmod +x run_pipeline.sh
#   ./run_pipeline.sh              # フル実行（約3〜4時間）
#   ./run_pipeline.sh --test       # テスト実行（約10分）
#   ./run_pipeline.sh --from-dpo   # DPOから再開
#
# メモリ使用量の目安:
#   SFT学習中: ~12GB（他アプリに24GB残る）
#   DPO学習中: ~14GB（他アプリに22GB残る）
#   推論/評価:  ~5GB（他アプリに31GB残る）
# =============================================================================

set -euo pipefail

TEST_MODE=false
START_STEP=1

# 引数解析
for arg in "$@"; do
  case $arg in
    --test)       TEST_MODE=true ;;
    --from-sft)   START_STEP=2 ;;
    --from-dpo)   START_STEP=3 ;;
    --from-eval)  START_STEP=4 ;;
    --help|-h)
      grep "^#" "$0" | head -20 | sed 's/^# //'
      exit 0
      ;;
  esac
done

# カラー出力
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

log_step() { echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"; echo -e "${GREEN}[Step $1] $2${NC}"; echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"; }
log_info() { echo -e "${YELLOW}▶ $1${NC}"; }
log_done() { echo -e "${GREEN}✓ $1${NC}"; }
log_error() { echo -e "${RED}✗ $1${NC}"; }

# テストフラグ
TEST_FLAG=""
if $TEST_MODE; then
  TEST_FLAG="--test-run"
  log_info "テストモード: 各ステップを短縮実行します"
fi

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║     Qwen3-8B 日本語LoRA+DPO フルパイプライン            ║"
echo "║     MacBook Pro M4 Max 36GB                              ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "  Step 1: データセット準備（品質フィルタリング + DPOデータ生成）"
echo "  Step 2: SFT LoRAファインチューニング"
echo "  Step 3: DPO 日本語品質向上トレーニング"
echo "  Step 4: 評価（ベースモデルとの比較）"
echo ""

START_TIME=$(date +%s)

# =============================================================================
# Step 1: データセット準備
# =============================================================================
if [ "$START_STEP" -le 1 ]; then
  log_step 1 "データセット準備"
  log_info "日本語データセットをダウンロード・フィルタリング・整形します"

  python 1_prepare_dataset.py

  if [ ! -f "./data/train.jsonl" ]; then
    log_error "train.jsonl が生成されませんでした"
    exit 1
  fi
  TRAIN_COUNT=$(wc -l < ./data/train.jsonl)
  DPO_COUNT=$(wc -l < ./data/dpo/train.jsonl 2>/dev/null || echo "0")
  log_done "SFTデータ: ${TRAIN_COUNT}件 / DPOデータ: ${DPO_COUNT}件"
fi

# =============================================================================
# Step 2: SFT LoRAファインチューニング
# =============================================================================
if [ "$START_STEP" -le 2 ]; then
  log_step 2 "SFT LoRAファインチューニング"
  log_info "推定時間: $(if $TEST_MODE; then echo '5分'; else echo '1〜2時間'; fi)"
  log_info "メモリ使用量: ~12GB（他アプリに24GB残ります）"

  python 2_finetune.py $TEST_FLAG --config config.yaml

  if [ ! -d "./adapters/japanese-v1" ] && ! $TEST_MODE; then
    log_error "SFTアダプターが生成されませんでした"
    exit 1
  fi
  log_done "SFT完了: ./adapters/japanese-v1"
fi

# =============================================================================
# Step 3: DPO 品質向上トレーニング
# =============================================================================
if [ "$START_STEP" -le 3 ]; then
  log_step 3 "DPO 日本語品質向上トレーニング"
  log_info "SFT済みモデルに対して敬語・文体・自然さを最適化します"
  log_info "推定時間: $(if $TEST_MODE; then echo '2分'; else echo '30〜60分'; fi)"
  log_info "メモリ使用量: ~14GB（他アプリに22GB残ります）"

  python 2b_dpo.py $TEST_FLAG --config config.yaml

  log_done "DPO完了: ./adapters/japanese-dpo"
fi

# =============================================================================
# Step 4: 評価
# =============================================================================
if [ "$START_STEP" -le 4 ]; then
  log_step 4 "日本語能力評価（ベースモデルとの比較）"
  log_info "評価カテゴリ: 文法・語彙・コーディング・文章執筆・指示理解・敬語・文体一貫性"

  # DPO済みモデルを使用（存在する場合）
  if [ -d "./models/qwen3-8b-japanese-dpo" ]; then
    python 3_evaluate.py --compare \
      --model-path ./models/qwen3-8b-japanese-dpo \
      --base-model-path ./models/qwen3-8b
  elif [ -d "./models/qwen3-8b-japanese" ]; then
    python 3_evaluate.py --compare \
      --model-path ./models/qwen3-8b-japanese \
      --base-model-path ./models/qwen3-8b
  else
    python 3_evaluate.py --compare --use-adapter \
      --adapter-path ./adapters/japanese-dpo \
      --base-model-path ./models/qwen3-8b
  fi

  log_done "評価完了: ./eval_results/comparison.json"
fi

# =============================================================================
# 完了サマリー
# =============================================================================
END_TIME=$(date +%s)
ELAPSED=$(( (END_TIME - START_TIME) / 60 ))

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║                   パイプライン完了！                     ║"
printf "║  総経過時間: %-43s║\n" "${ELAPSED}分"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "  生成物:"
echo "  ├── models/qwen3-8b-japanese-dpo/  （最終モデル）"
echo "  ├── adapters/japanese-v1/           （SFTアダプター）"
echo "  ├── adapters/japanese-dpo/          （DPOアダプター）"
echo "  └── eval_results/comparison.json   （評価レポート）"
echo ""
echo "  Ollamaで使う場合:"
echo "    ollama create qwen3-japanese -f Modelfile"
echo "    ollama run qwen3-japanese"
echo ""
