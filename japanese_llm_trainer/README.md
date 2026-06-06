# 日本語LLM ファインチューニング・パイプライン

MacBook Pro M4 Max 36GB向けの、ローカルLLM日本語能力強化パイプラインです。
Qwen3-8B / 14B に対応し、SFT → DPO/ORPO → SPIN → 高度な最適化までを一気通貫で行えます。

## 環境要件

- macOS (Apple Silicon: M1/M2/M3/M4)
- Python 3.11以上
- 36GB 統合メモリ推奨（最低16GB）

## セットアップ

```bash
cd japanese_llm_trainer
pip install -r requirements.txt
huggingface-cli login   # HuggingFaceアカウントが必要
```

## クイックスタート（推奨フロー）

```bash
# 14B + ORPO + SPIN を一括実行（就寝中に推奨・約10時間）
./run_pipeline.sh --model 14b --spin --use-llm-judge

# まず動作確認（約15分）
./run_pipeline.sh --model 14b --spin --test
```

---

## 手法の全体像

```
[基礎パイプライン]
  1. データ準備（品質フィルタ + カリキュラム学習 + DPOデータ生成）
  2. SFT LoRA    ─┐
  2b. DPO         ├─ または ─→ 2c. ORPO（SFT+DPOを1ステップ）
  3. 評価（ルールベース + ローカルLLM-as-Judge）

[反復改善]
  4. SPIN（生成→採点→ペア化→ORPO を自動収束まで繰り返す）

[高度な品質最適化]
  2d. NEFTune（埋め込みノイズで指示追従性 +5〜10%）
  spectrum_analyze（高SNRレイヤーのみ学習）
  5. モデルマージ（TIES/DARE/SLERP で複数モデルを融合）
```

---

## 基礎パイプライン

### Step 1: データセット準備
```bash
python 1_prepare_dataset.py            # カリキュラム学習ソート付き
python 1_prepare_dataset.py --no-curriculum   # ランダム順
```

### Step 2: 学習（8Bは SFT+DPO、14Bは ORPO 推奨）
```bash
# 8B: SFT → DPO
python 2_finetune.py --config config.yaml
python 2b_dpo.py --config config.yaml

# 14B: ORPO（SFT+DPO統合）
python 2c_orpo.py --config config_14b.yaml
```

### Step 3: 評価
```bash
python 3_evaluate.py --compare                  # ルールベース
python 3_evaluate.py --compare --use-llm-judge  # + ローカルLLM審査
```

---

## 反復改善（SPIN）

```bash
python 4_spin.py --config config_14b.yaml --iterations 5
python 4_spin.py --from-iteration 3   # 途中再開
```
生成→採点→学習を繰り返し、Judge改善が閾値未満になると自動収束します。

---

## 高度な品質最適化

### NEFTune（埋め込みノイズ）
学習時に埋め込みへ微小ノイズを加え、指示追従性を向上させます。
```bash
python 2d_train_neftune.py --config config_14b.yaml
```
`config_14b.yaml` の `neftune.neftune_alpha`（推奨5.0）で強度を調整。

### Spectrum（高SNRレイヤー選択学習）
情報量の多いレイヤーだけを学習し、効率と品質を両立します。
```bash
# 1. SNR解析（上位30%のレイヤーを選択）
python spectrum_analyze.py --model ./models/qwen3-14b --top-fraction 0.3

# 2. 選択レイヤーのみ学習（NEFTuneと併用可）
python 2d_train_neftune.py --config config_14b.yaml --spectrum-layers spectrum_layers.json
```

### モデルマージ（TIES / DARE / SLERP）
複数のモデルの長所を1つに統合します（学習不要・数分）。
```bash
pip install mergekit

# SPINチェックポイントを自動収集してTIESで融合
python 5_merge.py --auto-spin --method ties --to-mlx

# レシピで手動融合（自作モデル + 他の日本語特化モデルなど）
python 5_merge.py --recipe merge_recipe.yaml --to-mlx
```

---

## ファイル構成

```
japanese_llm_trainer/
├── 1_prepare_dataset.py   # データ準備（品質フィルタ/カリキュラム/DPOデータ）
├── 2_finetune.py          # SFT LoRA（8B）
├── 2b_dpo.py              # DPO（8B）
├── 2c_orpo.py             # ORPO（14B・SFT+DPO統合）
├── 2d_train_neftune.py    # NEFTune + Spectrum対応 in-process学習
├── 3_evaluate.py          # 評価（ルールベース + LLM-as-Judge）
├── 4_spin.py              # SPIN反復自己改善
├── 5_merge.py             # モデルマージ
├── neftune.py             # NEFTuneノイズモジュール
├── spectrum_analyze.py    # SNRレイヤー解析
├── run_pipeline.sh        # 全工程オーケストレーション
├── config.yaml            # 8B設定
├── config_14b.yaml        # 14B設定（ORPO/SPIN/NEFTune込み）
├── merge_recipe.yaml      # マージレシピ例
├── data/                  # 学習データ（自動生成）
├── models/                # モデルファイル
├── adapters/              # LoRAアダプター
└── eval_results/          # 評価結果
```

---

## メモリ使用量の目安（M4 Max 36GB）

| フェーズ | 8B | 14B | 他アプリ用空き |
|---------|-----|------|--------------|
| 推論・評価 | ~5GB | ~9GB | 27GB以上 |
| SFT/ORPO学習 | ~12GB | ~21GB | 15GB以上 |
| DPO学習 | ~14GB | ~21GB | 15GB以上 |
| SPIN（学習フェーズ） | ~14GB | ~21GB | 15GB以上 |
| マージ | CPU/メモリのみ | 〜20GB | 16GB以上 |

学習は順番に実行されるため、複数が同時にメモリを占有することはありません。

---

## 期待される品質（Sonnet=100%とした相対値・推定）

| 構成 | 相対品質 |
|------|---------|
| Qwen3-14B 素のまま | ~65% |
| + ORPO | ~75% |
| + SPIN（収束後） | ~78% |
| + NEFTune + マージ | ~80〜82% |
| + Spectrum | ~82〜84%（14Bの実質的な天井） |

執筆・文章系タスクでは85〜90%まで到達可能ですが、複雑な推論・広範な知識は
パラメータ規模の壁があり、これ以上はモデルサイズ自体の変更が必要です。

---

## Ollamaで使う場合

```bash
cat > Modelfile << 'EOF'
FROM ./models/qwen3-14b-japanese-orpo
SYSTEM "あなたは優秀な日本語AIアシスタントです。"
EOF

ollama create qwen3-japanese -f Modelfile
ollama run qwen3-japanese
```
