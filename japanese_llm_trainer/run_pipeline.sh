#!/bin/bash
# =============================================================================
# 日本語ファインチューニング フルパイプライン
# MacBook Pro M4 Max 36GB 向け
#
# 使い方:
#   ./run_pipeline.sh                        # 8B + SFT+DPO
#   ./run_pipeline.sh --model 14b            # 14B + ORPO
#   ./run_pipeline.sh --model 14b --spin     # 14B + ORPO + SPIN（推奨）
#   ./run_pipeline.sh --test                 # テスト実行（約15分）
#   ./run_pipeline.sh --from-spin            # SPINから再開
#   ./run_pipeline.sh --use-llm-judge        # 評価にLLM-as-Judgeを使用
#
# メモリ使用量:
#   14B ORPO学習: ~21GB / SPIN学習: ~21GB / 推論: ~9GB
#   ※ 他アプリは常に15GB以上利用可能
#
# 所要時間（14B + ORPO + SPIN 5回）:
#   データ準備: ~10分 / ORPO: ~2〜3時間
#   SPIN 1回:  ~90分（生成40分 + 採点30分 + 学習20分）
#   SPIN 5回:  ~7〜8時間（就寝中に実行推奨）
# =============================================================================

set -euo pipefail

# デフォルト設定
MODEL_SIZE="8b"
USE_ORPO=false
USE_SPIN=false
TEST_MODE=false
USE_LLM_JUDGE=false
START_STEP=1
SPIN_ITERATIONS=5

# 引数解析
for arg in "$@"; do
  case $arg in
    --model=14b|--14b)  MODEL_SIZE="14b" ;;
    --model=8b|--8b)    MODEL_SIZE="8b" ;;
    --orpo)        USE_ORPO=true ;;
    --spin)        USE_SPIN=true ;;
    --spin=*)      USE_SPIN=true; SPIN_ITERATIONS="${arg#*=}" ;;
    --test)        TEST_MODE=true ;;
    --use-llm-judge) USE_LLM_JUDGE=true ;;
    --from-data)   START_STEP=1 ;;
    --from-sft)    START_STEP=2 ;;
    --from-dpo)    START_STEP=3 ;;
    --from-orpo)   START_STEP=3 ;;
    --from-eval)   START_STEP=4 ;;
    --from-spin)   START_STEP=5 ;;
    --help|-h)
      grep "^#" "$0" | head -25 | sed 's/^# //'
      exit 0
      ;;
  esac
done

# 14Bを選択した場合はデフォルトでORPOを使用
if [ "$MODEL_SIZE" = "14b" ]; then
  USE_ORPO=true
fi

# 設定ファイルとパスを決定
if [ "$MODEL_SIZE" = "14b" ]; then
  CONFIG="config_14b.yaml"
  BASE_MODEL="./models/qwen3-14b"
  HF_MODEL="Qwen/Qwen3-14B"
  FINAL_MODEL="./models/qwen3-14b-japanese-orpo"
  SFT_ADAPTER="./adapters/14b-japanese-v1"
  ORPO_ADAPTER="./adapters/14b-japanese-orpo"
else
  CONFIG="config.yaml"
  BASE_MODEL="./models/qwen3-8b"
  HF_MODEL="Qwen/Qwen3-8B"
  FINAL_MODEL="./models/qwen3-8b-japanese-dpo"
  SFT_ADAPTER="./adapters/japanese-v1"
  ORPO_ADAPTER="./adapters/japanese-orpo"
fi

# カラー出力
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

log_step() {
  echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
  echo -e "${GREEN}[Step $1] $2${NC}"
  echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}
log_info()  { echo -e "${YELLOW}▶ $1${NC}"; }
log_done()  { echo -e "${GREEN}✓ $1${NC}"; }
log_error() { echo -e "${RED}✗ $1${NC}"; }
log_mem()   { echo -e "${CYAN}  メモリ: $1${NC}"; }

TEST_FLAG=""
if $TEST_MODE; then
  TEST_FLAG="--test-run"
fi

# バナー表示
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
if [ "$MODEL_SIZE" = "14b" ]; then
  echo "║   Qwen3-14B 日本語 ORPO パイプライン（高品質版）         ║"
  echo "║   MacBook Pro M4 Max 36GB                                ║"
  echo "╠══════════════════════════════════════════════════════════╣"
  echo "║   期待向上効果: 8Bベース比 +10〜20%                      ║"
  echo "║   特に: 敬語+15〜20% / 複合指示+15〜25%                  ║"
else
  echo "║   Qwen3-8B 日本語 SFT+DPO パイプライン                   ║"
  echo "║   MacBook Pro M4 Max 36GB                                ║"
fi
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
echo "  設定: モデル=${MODEL_SIZE}B | ORPO=$( $USE_ORPO && echo ON || echo OFF ) | SPIN=$( $USE_SPIN && echo "${SPIN_ITERATIONS}回" || echo OFF ) | LLM-Judge=$( $USE_LLM_JUDGE && echo ON || echo OFF )"
echo ""
echo "  Step 1: データ準備（品質フィルタ + カリキュラム学習ソート + DPOデータ）"
if $USE_ORPO; then
  echo "  Step 2: ORPO トレーニング（SFT + DPO を1ステップで実行）"
else
  echo "  Step 2: SFT LoRA ファインチューニング"
  echo "  Step 3: DPO 日本語品質向上トレーニング"
fi
if $USE_LLM_JUDGE; then
  echo "  Step 4: 評価（ルールベース + LLM-as-Judge）"
else
  echo "  Step 4: 評価（ルールベース）"
fi
if $USE_SPIN; then
  echo "  Step 5: SPIN 反復自己改善（最大${SPIN_ITERATIONS}回 → 自動収束）"
fi
echo ""

START_TIME=$(date +%s)

# =============================================================================
# Step 1: データセット準備（カリキュラム学習）
# =============================================================================
if [ "$START_STEP" -le 1 ]; then
  log_step 1 "データセット準備（カリキュラム学習ソート付き）"
  log_info "日本語データセットをダウンロード・フィルタリング・難易度順にソートします"
  log_mem "CPU処理のみ / GPU不使用"

  python 1_prepare_dataset.py --curriculum

  if [ ! -f "./data/train.jsonl" ]; then
    log_error "train.jsonl が生成されませんでした"
    exit 1
  fi
  TRAIN_COUNT=$(wc -l < ./data/train.jsonl)
  DPO_COUNT=$(wc -l < ./data/dpo/train.jsonl 2>/dev/null || echo "0")
  log_done "SFTデータ: ${TRAIN_COUNT}件 / DPOデータ: ${DPO_COUNT}件（難易度順ソート済み）"
fi

# モデルのダウンロード確認
if [ ! -d "$BASE_MODEL" ]; then
  log_step "0" "モデルダウンロード"
  log_info "モデルをダウンロードします: ${HF_MODEL}"
  log_info "サイズ: $( [ "$MODEL_SIZE" = "14b" ] && echo "~28GB" || echo "~15GB" )"
  mkdir -p "$BASE_MODEL"
  huggingface-cli download "$HF_MODEL" --local-dir "$BASE_MODEL" --exclude "*.pt"
  log_done "ダウンロード完了: ${BASE_MODEL}"
fi

# =============================================================================
# Step 2/3: トレーニング
# =============================================================================
if $USE_ORPO; then
  # ORPOモード（14B向け）
  if [ "$START_STEP" -le 3 ]; then
    log_step 2 "ORPO トレーニング（SFT + DPO 統合）"
    log_info "推定時間: $( $TEST_MODE && echo '3分' || echo '2〜3時間（14B）' )"
    log_mem "~20GB（他アプリに16GB残ります）"

    python 2c_orpo.py $TEST_FLAG --config "$CONFIG"

    log_done "ORPO完了: ${ORPO_ADAPTER}"
  fi
else
  # 通常モード（8B向け）
  if [ "$START_STEP" -le 2 ]; then
    log_step 2 "SFT LoRA ファインチューニング"
    log_info "推定時間: $( $TEST_MODE && echo '5分' || echo '1〜2時間（8B）' )"
    log_mem "~12GB（他アプリに24GB残ります）"

    python 2_finetune.py $TEST_FLAG --config "$CONFIG"

    if [ ! -d "$SFT_ADAPTER" ] && ! $TEST_MODE; then
      log_error "SFTアダプターが生成されませんでした"
      exit 1
    fi
    log_done "SFT完了: ${SFT_ADAPTER}"
  fi

  if [ "$START_STEP" -le 3 ]; then
    log_step 3 "DPO 日本語品質向上トレーニング"
    log_info "推定時間: $( $TEST_MODE && echo '2分' || echo '30〜60分（8B）' )"
    log_mem "~14GB（他アプリに22GB残ります）"

    python 2b_dpo.py $TEST_FLAG --config "$CONFIG"

    log_done "DPO完了"
  fi
fi

# =============================================================================
# Step 4: 評価
# =============================================================================
if [ "$START_STEP" -le 4 ]; then
  log_step 4 "日本語能力評価"

  JUDGE_FLAG=""
  if $USE_LLM_JUDGE; then
    log_info "LLM-as-Judge モード: ローカルモデルが採点します（追加時間: +30〜60分）"
    log_mem "~9GB（推論のみ）"
    JUDGE_FLAG="--use-llm-judge --judge-model ${BASE_MODEL}"
  fi

  # 最終モデルパスを決定
  if $USE_ORPO; then
    EVAL_MODEL="${FINAL_MODEL}"
    BASE_FOR_COMPARE="${BASE_MODEL}"
  else
    EVAL_MODEL="${FINAL_MODEL}"
    BASE_FOR_COMPARE="${BASE_MODEL}"
  fi

  if [ -d "$EVAL_MODEL" ]; then
    python 3_evaluate.py --compare \
      --model-path "$EVAL_MODEL" \
      --base-model-path "$BASE_FOR_COMPARE" \
      $JUDGE_FLAG
  else
    # マージ前のアダプター使用
    ADAPTER=$( $USE_ORPO && echo "$ORPO_ADAPTER" || echo "$SFT_ADAPTER" )
    python 3_evaluate.py --compare --use-adapter \
      --adapter-path "$ADAPTER" \
      --base-model-path "$BASE_FOR_COMPARE" \
      $JUDGE_FLAG
  fi

  log_done "評価完了: ./eval_results/comparison.json"
fi

# =============================================================================
# Step 5: SPIN 反復自己改善
# =============================================================================
if $USE_SPIN && [ "$START_STEP" -le 5 ]; then
  log_step 5 "SPIN 反復自己改善ループ（最大${SPIN_ITERATIONS}回）"
  log_info "生成 → 採点 → Preferenceペア生成 → ORPO を繰り返します"
  log_info "推定時間: $( $TEST_MODE && echo '5分' || echo "SPIN1回あたり約90分 × 最大${SPIN_ITERATIONS}回（自動収束）" )"
  log_mem "生成・採点: ~9GB / ORPO学習: ~21GB（フェーズごとに変動）"

  SPIN_TEST_FLAG=""
  if $TEST_MODE; then
    SPIN_TEST_FLAG="--test-run"
  fi

  python 4_spin.py \
    --config "$CONFIG" \
    --iterations "$SPIN_ITERATIONS" \
    --judge-model "$BASE_MODEL" \
    $SPIN_TEST_FLAG

  # SPIN最終モデルを評価（収束後）
  SPIN_STATE_FILE="./data/spin/spin_state.json"
  if [ -f "$SPIN_STATE_FILE" ]; then
    SPIN_FINAL_MODEL=$(python3 -c "
import json
with open('$SPIN_STATE_FILE') as f:
    s = json.load(f)
print(s.get('current_model', ''))
")
    if [ -n "$SPIN_FINAL_MODEL" ] && [ -d "$SPIN_FINAL_MODEL" ]; then
      log_info "SPIN最終モデルを評価中: $SPIN_FINAL_MODEL"
      JUDGE_FLAG=""
      if $USE_LLM_JUDGE; then
        JUDGE_FLAG="--use-llm-judge --judge-model ${BASE_MODEL}"
      fi
      python 3_evaluate.py --compare \
        --model-path "$SPIN_FINAL_MODEL" \
        --base-model-path "$BASE_MODEL" \
        --output-dir "./eval_results/after_spin" \
        $JUDGE_FLAG
    fi
  fi

  log_done "SPIN完了"
fi

# =============================================================================
# 完了サマリー
# =============================================================================
END_TIME=$(date +%s)
ELAPSED=$(( (END_TIME - START_TIME) / 60 ))

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║                   パイプライン完了！                     ║"
printf "║  モデル: %-49s║\n" "Qwen3-${MODEL_SIZE}B"
printf "║  経過時間: %-47s║\n" "${ELAPSED}分"
echo "╠══════════════════════════════════════════════════════════╣"
echo "║  生成物:                                                 ║"
if $USE_SPIN; then
  echo "║    最終モデル: data/spin/spin_state.json 参照            ║"
  echo "║    SPIN評価:   eval_results/after_spin/comparison.json  ║"
else
  printf "║    最終モデル: %-43s║\n" "$(basename $FINAL_MODEL)/"
  echo "║    評価結果:   eval_results/comparison.json              ║"
fi
echo "╠══════════════════════════════════════════════════════════╣"
echo "║  Ollamaで使う場合:                                       ║"
echo "║    ollama create qwen3-ja -f Modelfile                   ║"
echo "║    ollama run qwen3-ja                                   ║"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""
