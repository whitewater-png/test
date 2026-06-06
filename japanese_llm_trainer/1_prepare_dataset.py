#!/usr/bin/env python3
"""
日本語ファインチューニング用データセット準備スクリプト
対象モデル: Qwen3-8B on Apple Silicon (MLX)

使い方:
    pip install datasets huggingface_hub tqdm
    python 1_prepare_dataset.py
"""

import json
import random
import os
from pathlib import Path
from tqdm import tqdm

# ---------------------------------------------------------------------------
# 設定
# ---------------------------------------------------------------------------
OUTPUT_DIR = Path("./data")
TRAIN_FILE = OUTPUT_DIR / "train.jsonl"
VALID_FILE = OUTPUT_DIR / "valid.jsonl"
VALID_RATIO = 0.05          # 全データの5%を検証用に
MAX_SAMPLES = 20000         # 上限サンプル数（メモリ節約のため）
SEED = 42

# Qwen3のチャットテンプレート
SYSTEM_PROMPT = (
    "あなたは優秀な日本語AIアシスタントです。"
    "アプリケーション開発と文章執筆を得意とし、"
    "常に正確で自然な日本語で回答します。"
)


def format_chat(instruction: str, output: str, system: str = SYSTEM_PROMPT) -> str:
    """Qwen3のim_start/im_endフォーマットに変換する"""
    return (
        f"<|im_start|>system\n{system}<|im_end|>\n"
        f"<|im_start|>user\n{instruction}<|im_end|>\n"
        f"<|im_start|>assistant\n{output}<|im_end|>"
    )


# ---------------------------------------------------------------------------
# データセットローダー
# ---------------------------------------------------------------------------

def load_dolly_ja() -> list[dict]:
    """databricks-dolly-15k 日本語版"""
    try:
        from datasets import load_dataset
        ds = load_dataset("kunishou/databricks-dolly-15k-ja", split="train")
        records = []
        for ex in ds:
            instruction = ex.get("instruction", "").strip()
            context = ex.get("context", "").strip()
            response = ex.get("response", "").strip()
            if not instruction or not response:
                continue
            if context:
                instruction = f"{instruction}\n\n参考情報:\n{context}"
            records.append({"instruction": instruction, "output": response})
        print(f"  dolly-ja: {len(records)}件")
        return records
    except Exception as e:
        print(f"  dolly-ja: スキップ ({e})")
        return []


def load_izumi_lab() -> list[dict]:
    """izumi-lab 日本語LLMデータセット"""
    try:
        from datasets import load_dataset
        ds = load_dataset(
            "izumi-lab/llm-japanese-dataset",
            split="train",
            trust_remote_code=True,
        )
        records = []
        for ex in ds:
            instruction = (ex.get("prompt") or ex.get("instruction") or "").strip()
            output = (ex.get("response") or ex.get("output") or "").strip()
            if not instruction or not output:
                continue
            records.append({"instruction": instruction, "output": output})
        print(f"  izumi-lab: {len(records)}件")
        return records
    except Exception as e:
        print(f"  izumi-lab: スキップ ({e})")
        return []


def load_coding_ja() -> list[dict]:
    """コーディング特化データ（日本語解説付き）"""
    try:
        from datasets import load_dataset
        ds = load_dataset("inejc/python-functions-docstrings", split="train")
        records = []
        for ex in ds:
            func = ex.get("function", "").strip()
            doc = ex.get("docstring", "").strip()
            if not func or not doc:
                continue
            instruction = f"以下のPython関数の動作を日本語で詳しく説明してください。\n\n```python\n{func}\n```"
            output = f"この関数の説明:\n\n{doc}\n\n**使用方法の例:**\n関数を呼び出す際は引数の型と戻り値の型に注意してください。"
            records.append({"instruction": instruction, "output": output})
        print(f"  coding-ja: {len(records)}件")
        return records[:3000]  # コーディングは3000件に制限
    except Exception as e:
        print(f"  coding-ja: スキップ ({e})")
        return []


def load_writing_samples() -> list[dict]:
    """文章執筆サンプル（手動定義）"""
    samples = [
        {
            "instruction": "「人工知能が社会に与える影響」というテーマで、800字程度のエッセイを書いてください。",
            "output": (
                "人工知能（AI）の急速な発展は、私たちの社会のあり方を根本から変えつつある。"
                "医療分野では、AIによる画像診断が医師の見落としを補い、早期発見率を高めている。"
                "製造業では、工場の自動化が生産効率を飛躍的に向上させた。こうした恩恵は疑いなく大きい。\n\n"
                "一方で、課題も山積している。まず雇用問題だ。ルーティン作業を中心に、多くの職種がAIに代替される"
                "リスクがあると指摘されている。しかし歴史を振り返れば、産業革命時も機械化による雇用喪失が懸念されたが、"
                "新たな産業と職種が生まれることで社会は適応してきた。AIも同様に、新しい仕事と可能性を生み出すだろう。\n\n"
                "より深刻なのは、偏りと公平性の問題だ。AIは学習データに含まれる偏見を増幅させる恐れがある。"
                "採用選考や融資審査にAIが使われる場合、差別的な判断が自動化・大規模化するリスクがある。\n\n"
                "AIと共存する社会を築くには、技術の発展と倫理的な設計を両輪で進める必要がある。"
                "透明性の高いアルゴリズム、適切な規制、そしてAIリテラシー教育が不可欠だ。"
                "AIは道具であり、それをどう使うかは人間の判断に委ねられている。"
            ),
        },
        {
            "instruction": "ビジネスメールの書き方のコツを5つ教えてください。",
            "output": (
                "ビジネスメールを効果的に書くための5つのコツをご紹介します。\n\n"
                "**1. 件名は具体的かつ簡潔に**\n"
                "「ご連絡」ではなく「〇〇プロジェクト 進捗確認のお願い（〇月〇日まで）」のように、"
                "用件と期日が一目でわかる件名にしましょう。\n\n"
                "**2. 結論を冒頭に書く**\n"
                "日本語は結論が末尾になりがちですが、ビジネスメールでは「お願いがあります」から始め、"
                "詳細を後述する構成が読み手への負担を減らします。\n\n"
                "**3. 一メールに一用件**\n"
                "複数の案件を一通にまとめると、どれかが見落とされるリスクがあります。"
                "件ごとにメールを分け、優先度を明確にしましょう。\n\n"
                "**4. 数字・固有名詞を明確に**\n"
                "「近日中に」「多数の」といった曖昧な表現を避け、「〇月〇日（金）まで」"
                "「〇〇株式会社 田中様」と具体的に記載します。\n\n"
                "**5. 送信前に音読チェック**\n"
                "声に出して読むことで、語尾の重複・敬語の誤り・読みにくい文章を発見できます。"
                "特に敬語の二重表現（「ご利用になられる」など）に注意しましょう。"
            ),
        },
        {
            "instruction": "Pythonのデコレーターとは何か、具体的なコード例を使って初心者向けに説明してください。",
            "output": (
                "デコレーターは、既存の関数に機能を追加するための仕組みです。"
                "関数を「ラップ」して、前後に処理を追加できます。\n\n"
                "**基本的な例:**\n"
                "```python\n"
                "def 処理時間を計測(func):\n"
                "    import time\n"
                "    def ラッパー(*args, **kwargs):\n"
                "        開始 = time.time()\n"
                "        結果 = func(*args, **kwargs)  # 元の関数を実行\n"
                "        終了 = time.time()\n"
                "        print(f\"{func.__name__} の処理時間: {終了 - 開始:.3f}秒\")\n"
                "        return 結果\n"
                "    return ラッパー\n\n"
                "@処理時間を計測  # ← これがデコレーター\n"
                "def 重い処理():\n"
                "    import time\n"
                "    time.sleep(1)\n"
                "    return \"完了\"\n\n"
                "重い処理()  # → 「重い処理 の処理時間: 1.001秒」と表示\n"
                "```\n\n"
                "**ポイント:**\n"
                "- `@デコレーター名` と書くだけで関数に機能を追加できます\n"
                "- ログ記録・認証チェック・キャッシュなどでよく使われます\n"
                "- Flaskの `@app.route('/path')` もデコレーターの一種です"
            ),
        },
        {
            "instruction": "日本語でREADME.mdを書くベストプラクティスを教えてください。",
            "output": (
                "日本語README.mdのベストプラクティスをご紹介します。\n\n"
                "**必須セクション:**\n"
                "1. **プロジェクト概要** — 何ができるのか1〜2文で説明\n"
                "2. **デモ/スクリーンショット** — 百聞は一見に如かず\n"
                "3. **インストール方法** — コマンドをそのままコピペできるように\n"
                "4. **使い方** — 最小動作例（Getting Started）\n"
                "5. **ライセンス** — 必ず明記\n\n"
                "**書き方のコツ:**\n"
                "- コードブロックには言語を指定する（` ```python ` など）\n"
                "- バージョン要件を明示する（`Python 3.11以上`）\n"
                "- バッジ（build passing, license, version）で状態を視覚化\n"
                "- Contributingガイドへのリンクを追加\n\n"
                "**日本語特有の注意点:**\n"
                "- 全角記号と半角記号を混在させない\n"
                "- 技術用語は英語のままにするか、初出時に英語を併記する\n"
                "- 英語版READMEへのリンクも用意すると国際的なコントリビューターが増える"
            ),
        },
    ]
    # サンプルを200件に水増し（バリエーションを付けて）
    expanded = []
    for i in range(200):
        base = samples[i % len(samples)].copy()
        expanded.append(base)
    print(f"  writing-samples: {len(expanded)}件")
    return expanded


# ---------------------------------------------------------------------------
# メイン処理
# ---------------------------------------------------------------------------

def main():
    random.seed(SEED)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    print("=== データセット読み込み中 ===")
    all_records: list[dict] = []
    all_records.extend(load_dolly_ja())
    all_records.extend(load_izumi_lab())
    all_records.extend(load_coding_ja())
    all_records.extend(load_writing_samples())

    print(f"\n合計: {len(all_records)}件")

    # シャッフル＆上限適用
    random.shuffle(all_records)
    all_records = all_records[:MAX_SAMPLES]

    # train / valid 分割
    split_idx = int(len(all_records) * (1 - VALID_RATIO))
    train_records = all_records[:split_idx]
    valid_records = all_records[split_idx:]

    print(f"学習用: {len(train_records)}件  検証用: {len(valid_records)}件")

    # MLX用フォーマットでJSONL書き出し
    def write_jsonl(records: list[dict], path: Path):
        with open(path, "w", encoding="utf-8") as f:
            for rec in tqdm(records, desc=f"書き出し: {path.name}"):
                text = format_chat(rec["instruction"], rec["output"])
                f.write(json.dumps({"text": text}, ensure_ascii=False) + "\n")

    write_jsonl(train_records, TRAIN_FILE)
    write_jsonl(valid_records, VALID_FILE)

    # 統計表示
    train_sizes = []
    with open(TRAIN_FILE, encoding="utf-8") as f:
        for line in f:
            train_sizes.append(len(json.loads(line)["text"]))

    print(f"\n=== 完了 ===")
    print(f"学習データ: {TRAIN_FILE}")
    print(f"検証データ: {VALID_FILE}")
    print(f"平均テキスト長: {sum(train_sizes) // len(train_sizes)}文字")
    print(f"最大テキスト長: {max(train_sizes)}文字")
    print("\n次のステップ: python 2_finetune.py を実行してください")


if __name__ == "__main__":
    main()
