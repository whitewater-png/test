# Qwen3-8B 日本語LoRAファインチューニング

MacBook Pro M4 Max 36GB向けの日本語能力強化パイプラインです。

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

## 実行手順

### Step 1: データセット準備

```bash
python 1_prepare_dataset.py
```

- `data/train.jsonl` と `data/valid.jsonl` を生成します
- 日本語instructionデータセットを自動ダウンロード・整形します

### Step 2: ファインチューニング

```bash
# まずテスト実行（50ステップ、約5分）
python 2_finetune.py --test-run

# 本番実行（1000ステップ、約1〜2時間）
python 2_finetune.py
```

**メモリ不足の場合：**
`config.yaml` の以下を調整してください：
- `batch_size: 2`（4 → 2）
- `max_seq_length: 1024`（2048 → 1024）
- `lora_layers: 8`（16 → 8）

### Step 3: 評価

```bash
# ファインチューニング済みモデルの単体評価
python 3_evaluate.py

# ベースモデルとの比較評価
python 3_evaluate.py --compare

# アダプターを直接使用（マージ前でも可）
python 3_evaluate.py --use-adapter
```

## ファイル構成

```
japanese_llm_trainer/
├── 1_prepare_dataset.py   # データセット準備
├── 2_finetune.py          # LoRAファインチューニング
├── 3_evaluate.py          # 評価・比較
├── config.yaml            # 学習設定
├── requirements.txt       # 依存パッケージ
├── data/                  # 学習データ（自動生成）
│   ├── train.jsonl
│   └── valid.jsonl
├── models/                # モデルファイル
│   ├── qwen3-8b/          # ベースモデル
│   └── qwen3-8b-japanese/ # ファインチューニング済み
├── adapters/              # LoRAアダプター
│   └── japanese-v1/
└── eval_results/          # 評価結果
    ├── results.json
    ├── base_results.json
    └── comparison.json
```

## Ollamaで使う場合

```bash
# ファインチューニング済みモデルをOllamaに登録
cat > Modelfile << 'EOF'
FROM ./models/qwen3-8b-japanese
SYSTEM "あなたは優秀な日本語AIアシスタントです。"
EOF

ollama create qwen3-japanese -f Modelfile
ollama run qwen3-japanese
```

## メモリ使用量の目安（M4 Max 36GB）

| フェーズ | 使用量 | 残り空き |
|---------|--------|---------|
| 推論のみ | ~5GB | ~31GB |
| LoRAファインチューニング | ~12GB | ~24GB |
| 他アプリ同時使用 | +8GB | ~16GB |
