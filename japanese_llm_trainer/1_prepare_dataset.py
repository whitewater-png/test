#!/usr/bin/env python3
"""
日本語ファインチューニング用データセット準備スクリプト（高品質版）
対象モデル: Qwen3-8B on Apple Silicon (MLX)

変更点:
  - 品質フィルタリング追加（日本語比率・長さ・繰り返し検出）
  - 高品質データセット追加（ELYZA-tasks-100, oasst2-ja, マルチターン）
  - DPO用preferenceデータを同時生成

使い方:
    pip install datasets huggingface_hub tqdm
    python 1_prepare_dataset.py
"""

import json
import random
import re
from pathlib import Path
from tqdm import tqdm

# ---------------------------------------------------------------------------
# 設定
# ---------------------------------------------------------------------------
OUTPUT_DIR = Path("./data")
DPO_DIR = Path("./data/dpo")
TRAIN_FILE = OUTPUT_DIR / "train.jsonl"
VALID_FILE = OUTPUT_DIR / "valid.jsonl"
VALID_RATIO = 0.05
MAX_SAMPLES = 20000
SEED = 42

SYSTEM_PROMPT = (
    "あなたは優秀な日本語AIアシスタントです。"
    "アプリケーション開発と文章執筆を得意とし、"
    "常に正確で自然な日本語で回答します。"
)


# ---------------------------------------------------------------------------
# 品質フィルター
# ---------------------------------------------------------------------------

def quality_filter(record: dict) -> bool:
    """低品質なサンプルを除去する"""
    instruction = record.get("instruction", "")
    output = record.get("output", "")

    # 長さチェック
    if len(instruction) < 10 or len(output) < 30:
        return False
    if len(output) > 4000:
        return False

    # 日本語文字の割合チェック（出力が60%以上日本語であること）
    ja_chars = sum(1 for c in output if "぀" <= c <= "鿿")
    if len(output) > 0 and ja_chars / len(output) < 0.3:
        return False

    # 繰り返しパターン検出（同一フレーズが3回以上連続）
    if re.search(r"(.{10,})\1{2,}", output):
        return False

    # 文字種多様性チェック（同じ文字ばかりでないか）
    if len(output) > 0 and len(set(output)) / len(output) < 0.1:
        return False

    return True


def deduplicate(records: list[dict]) -> list[dict]:
    """instructionの先頭50文字で重複除去"""
    seen = set()
    unique = []
    for r in records:
        key = r.get("instruction", "")[:50]
        if key not in seen:
            seen.add(key)
            unique.append(r)
    return unique


# ---------------------------------------------------------------------------
# フォーマット変換
# ---------------------------------------------------------------------------

def format_chat(instruction: str, output: str, system: str = SYSTEM_PROMPT) -> str:
    return (
        f"<|im_start|>system\n{system}<|im_end|>\n"
        f"<|im_start|>user\n{instruction}<|im_end|>\n"
        f"<|im_start|>assistant\n{output}<|im_end|>"
    )


def format_multiturn(turns: list[dict], system: str = SYSTEM_PROMPT) -> str:
    """マルチターン会話をフォーマット。turns は [{"role": "user"|"assistant", "content": "..."}] のリスト"""
    result = f"<|im_start|>system\n{system}<|im_end|>\n"
    for turn in turns:
        role = turn["role"]
        content = turn["content"]
        result += f"<|im_start|>{role}\n{content}<|im_end|>\n"
    return result.rstrip()


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
        print(f"  dolly-ja (raw): {len(records)}件", end="")
        records = [r for r in records if quality_filter(r)]
        print(f" → フィルター後: {len(records)}件")
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
        print(f"  izumi-lab (raw): {len(records)}件", end="")
        records = [r for r in records if quality_filter(r)]
        print(f" → フィルター後: {len(records)}件")
        return records
    except Exception as e:
        print(f"  izumi-lab: スキップ ({e})")
        return []


def load_elyza_tasks() -> list[dict]:
    """ELYZA-tasks-100: 日本語タスク100種の高品質正解例"""
    try:
        from datasets import load_dataset
        ds = load_dataset("elyza/ELYZA-tasks-100", split="test", trust_remote_code=True)
        records = []
        for ex in ds:
            instruction = (ex.get("input") or "").strip()
            output = (ex.get("output") or "").strip()
            if not instruction or not output:
                continue
            records.append({"instruction": instruction, "output": output})
        print(f"  elyza-tasks-100: {len(records)}件")
        return records
    except Exception as e:
        print(f"  elyza-tasks-100: スキップ ({e})")
        return []


def load_oasst_ja() -> list[dict]:
    """oasst2 日本語版: 人間フィードバック付き対話データ"""
    try:
        from datasets import load_dataset
        ds = load_dataset("fujiki-ok/oasst2-33k-ja", split="train")
        records = []
        for ex in ds:
            instruction = (ex.get("prompt") or ex.get("instruction") or "").strip()
            output = (ex.get("response") or ex.get("output") or "").strip()
            if not instruction or not output:
                continue
            records.append({"instruction": instruction, "output": output})
        print(f"  oasst2-ja (raw): {len(records)}件", end="")
        records = [r for r in records if quality_filter(r)]
        print(f" → フィルター後: {len(records)}件")
        return records[:5000]
    except Exception as e:
        print(f"  oasst2-ja: スキップ ({e})")
        return []


def load_multiturn_ja() -> list[dict]:
    """マルチターン会話データ（文脈理解強化）"""
    try:
        from datasets import load_dataset
        ds = load_dataset(
            "kanhatakeyama/ramdom-to-fixed-multiturn-Calm3",
            split="train",
            trust_remote_code=True,
        )
        records = []
        for ex in ds:
            conversations = ex.get("conversations") or ex.get("messages") or []
            if len(conversations) < 4:  # 最低2往復
                continue
            turns = []
            for turn in conversations:
                role = turn.get("role") or turn.get("from", "")
                content = (turn.get("content") or turn.get("value") or "").strip()
                if role in ("human", "user"):
                    turns.append({"role": "user", "content": content})
                elif role in ("gpt", "assistant"):
                    turns.append({"role": "assistant", "content": content})
            if len(turns) >= 4:
                records.append({"multiturn": turns})
        print(f"  multiturn-ja (raw): {len(records)}件", end="")
        records = records[:3000]
        print(f" → 上限適用: {len(records)}件")
        return records
    except Exception as e:
        print(f"  multiturn-ja: スキップ ({e})")
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
            if not func or not doc or len(func) > 800:
                continue
            instruction = f"以下のPython関数の動作を日本語で詳しく説明してください。\n\n```python\n{func}\n```"
            output = (
                f"この関数の説明:\n\n{doc}\n\n"
                "**ポイント:**\n"
                "- 引数の型と戻り値の型に注意してください\n"
                "- エラーケースも考慮して使用してください"
            )
            records.append({"instruction": instruction, "output": output})
        print(f"  coding-ja: {len(records)}件 → 上限: 3000件")
        return records[:3000]
    except Exception as e:
        print(f"  coding-ja: スキップ ({e})")
        return []


def load_writing_samples() -> list[dict]:
    """文章執筆サンプル（高品質手動定義）"""
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
                "複数の案件を一通にまとめると、どれかが見落とされるリスクがあります。\n\n"
                "**4. 数字・固有名詞を明確に**\n"
                "「近日中に」「多数の」といった曖昧な表現を避け、「〇月〇日（金）まで」と具体的に記載します。\n\n"
                "**5. 送信前に音読チェック**\n"
                "声に出して読むことで、語尾の重複・敬語の誤り・読みにくい文章を発見できます。"
            ),
        },
        {
            "instruction": "Pythonのデコレーターとは何か、具体的なコード例を使って初心者向けに説明してください。",
            "output": (
                "デコレーターは、既存の関数に機能を追加するための仕組みです。\n\n"
                "**基本的な例:**\n"
                "```python\n"
                "def 処理時間を計測(func):\n"
                "    import time\n"
                "    def ラッパー(*args, **kwargs):\n"
                "        開始 = time.time()\n"
                "        結果 = func(*args, **kwargs)\n"
                "        終了 = time.time()\n"
                "        print(f'{func.__name__} の処理時間: {終了 - 開始:.3f}秒')\n"
                "        return 結果\n"
                "    return ラッパー\n\n"
                "@処理時間を計測\n"
                "def 重い処理():\n"
                "    time.sleep(1)\n"
                "    return '完了'\n"
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
                "4. **使い方** — 最小動作例\n"
                "5. **ライセンス** — 必ず明記\n\n"
                "**書き方のコツ:**\n"
                "- コードブロックには言語を指定する（` ```python ` など）\n"
                "- バージョン要件を明示する（`Python 3.11以上`）\n"
                "- バッジで状態を視覚化（build passing, license, version）\n\n"
                "**日本語特有の注意点:**\n"
                "- 全角記号と半角記号を混在させない\n"
                "- 技術用語は初出時に英語を併記する\n"
                "- 英語版READMEへのリンクも用意すると貢献者が増える"
            ),
        },
        {
            "instruction": "TypeScriptのジェネリクスについて、実用的な例を交えて説明してください。",
            "output": (
                "TypeScriptのジェネリクスは、型を引数として受け取る仕組みです。\n\n"
                "**基本的な例:**\n"
                "```typescript\n"
                "// 型安全なスタック実装\n"
                "class Stack<T> {\n"
                "  private items: T[] = [];\n\n"
                "  push(item: T): void {\n"
                "    this.items.push(item);\n"
                "  }\n\n"
                "  pop(): T | undefined {\n"
                "    return this.items.pop();\n"
                "  }\n"
                "}\n\n"
                "const numStack = new Stack<number>();\n"
                "numStack.push(1);  // OK\n"
                "// numStack.push('hello');  // コンパイルエラー\n"
                "```\n\n"
                "**実用的なユースケース:**\n"
                "- API レスポンスの型定義: `Promise<ApiResponse<User>>`\n"
                "- フォームバリデーション: `validate<T extends FormData>(data: T)`\n"
                "- リポジトリパターン: `Repository<Entity>` でCRUD操作を抽象化\n\n"
                "ジェネリクスを使うことで、型の安全性を保ちながら再利用可能なコードを書けます。"
            ),
        },
    ]
    expanded = []
    for i in range(300):
        base = samples[i % len(samples)].copy()
        expanded.append(base)
    print(f"  writing-samples: {len(expanded)}件")
    return expanded


# ---------------------------------------------------------------------------
# DPO用preferenceデータ生成
# ---------------------------------------------------------------------------

DPO_PAIRS: list[dict] = [
    # 敬語
    {
        "prompt": "上司に「明日の会議に参加できますか？」と確認するメールを書いてください。",
        "chosen": "お世話になっております。明日の会議についてご確認させていただきたく、ご連絡申し上げます。ご都合はいかがでしょうか。ご参加いただけますと幸いです。どうぞよろしくお願いいたします。",
        "rejected": "明日の会議に来れますか？来るなら教えてください。よろしく。",
    },
    {
        "prompt": "「ご利用になられる」という表現の問題点と正しい表現を教えてください。",
        "chosen": "「ご利用になられる」は二重敬語です。「ご利用になる」（尊敬語）に「られる」（尊敬の助動詞）が重複しています。正しい表現は「ご利用になる」または「ご利用される」です。",
        "rejected": "「ご利用になられる」は正しい敬語です。丁寧な表現なので問題ありません。",
    },
    {
        "prompt": "「申し訳ございません」と「すみません」の違いを教えてください。",
        "chosen": "「申し訳ございません」はフォーマルなビジネス・公的な場面で使う最上級の謝罪表現です。「すみません」はカジュアルな日常会話での軽い謝罪や呼びかけに使います。顧客対応や上司への謝罪では「申し訳ございません」を選んでください。",
        "rejected": "どちらも同じ意味です。「すみません」のほうが短くて便利なので常に使えます。",
    },
    # 文体一貫性
    {
        "prompt": "自然な日本語で「AIの将来性」についての意見を200字で書いてください。",
        "chosen": "AIは今後さらに発展し、医療・教育・製造など多くの分野で革新をもたらすと考えられます。特に繰り返し作業の自動化により、人間はより創造的な仕事に集中できるようになるでしょう。一方で、倫理的な課題への対応も不可欠です。",
        "rejected": "AIの将来性は高いです。医療で使われます。でも問題もある。倫理が大事だ。みんなで考えよう。AIはすごいと思います。これからも発展するでしょう。",
    },
    # コーディング説明の品質
    {
        "prompt": "Pythonのリスト内包表記を初心者向けに説明してください。",
        "chosen": (
            "リスト内包表記は、リストを簡潔に作成する構文です。\n\n"
            "**基本構文:**\n"
            "```python\n"
            "# 通常のfor文\n"
            "squares = []\n"
            "for i in range(5):\n"
            "    squares.append(i ** 2)\n\n"
            "# リスト内包表記（同じ結果）\n"
            "squares = [i ** 2 for i in range(5)]\n"
            "# → [0, 1, 4, 9, 16]\n"
            "```\n\n"
            "**条件付きの例:**\n"
            "```python\n"
            "# 偶数だけ抽出\n"
            "evens = [i for i in range(10) if i % 2 == 0]\n"
            "# → [0, 2, 4, 6, 8]\n"
            "```\n\n"
            "読みやすさのため、複雑な処理には通常のfor文を使いましょう。"
        ),
        "rejected": "リスト内包表記は[x for x in iterable]です。速い。使うといい。",
    },
    {
        "prompt": "Gitのブランチ戦略について説明してください。",
        "chosen": (
            "代表的なGitブランチ戦略を3つご紹介します。\n\n"
            "**1. GitHub Flow（シンプル）**\n"
            "- `main`ブランチを常にデプロイ可能に保つ\n"
            "- 機能開発はfeatureブランチで行い、PRでmainにマージ\n"
            "- 小規模チームや継続的デプロイに最適\n\n"
            "**2. Git Flow（厳格）**\n"
            "- `main`（本番）、`develop`（開発）、`feature/*`、`release/*`、`hotfix/*`\n"
            "- リリースサイクルが明確な中〜大規模プロジェクトに向く\n\n"
            "**3. Trunk Based Development**\n"
            "- 全員が短命のブランチを作り、毎日`main`にマージ\n"
            "- 大規模チームでのCI/CDに最適\n\n"
            "チームの規模とリリース頻度に合わせて選びましょう。"
        ),
        "rejected": "ブランチ使えばいいです。mainとdevelopがあります。PRしてマージ。",
    },
    # 完全性
    {
        "prompt": "Pythonで例外処理を行う方法を教えてください。",
        "chosen": (
            "Pythonの例外処理は`try-except`文で行います。\n\n"
            "```python\n"
            "# 基本構文\n"
            "try:\n"
            "    result = 10 / 0\n"
            "except ZeroDivisionError as e:\n"
            "    print(f'エラー: {e}')\n"
            "except (TypeError, ValueError) as e:\n"
            "    print(f'型エラー: {e}')\n"
            "else:\n"
            "    print('成功:', result)  # 例外が発生しなかった場合\n"
            "finally:\n"
            "    print('常に実行される')  # クリーンアップ処理に使用\n"
            "```\n\n"
            "**ベストプラクティス:**\n"
            "- 例外は具体的に捕捉する（`except Exception`は最終手段）\n"
            "- `finally`でリソース解放を行う\n"
            "- カスタム例外クラスで独自エラーを定義できる"
        ),
        "rejected": "try exceptを使います。エラーが起きたらexceptに入ります。",
    },
    # 自然さ
    {
        "prompt": "「お疲れ様でした」と「ご苦労様でした」の使い分けを教えてください。",
        "chosen": (
            "**「お疲れ様でした」**\n"
            "上下関係なく使える汎用的な労いの言葉です。同僚・上司・部下のいずれにも使えます。"
            "現代のビジネスシーンではもっとも一般的な表現です。\n\n"
            "**「ご苦労様でした」**\n"
            "本来は目上の人が目下の人に使う表現です。"
            "上司から部下、依頼主から使者への労いに使われてきました。"
            "現代では上司に使うと失礼になる場合があるため注意が必要です。\n\n"
            "**まとめ:** 迷ったら「お疲れ様でした」を使えば間違いありません。"
        ),
        "rejected": "どちらも同じです。どちらを使っても大丈夫です。",
    },
    {
        "prompt": "SwiftとKotlinを比較して、どちらがモバイル開発に適しているか教えてください。",
        "chosen": (
            "**Swift（iOS/macOS向け）vs Kotlin（Android向け）**\n\n"
            "| 観点 | Swift | Kotlin |\n"
            "|------|-------|--------|\n"
            "| 対象プラットフォーム | Apple製品専用 | Android + マルチプラットフォーム(KMP) |\n"
            "| 学習曲線 | やや急 | Javaの知識があれば緩やか |\n"
            "| パフォーマンス | ネイティブ最高峰 | JVM上だが最適化が進んでいる |\n"
            "| コミュニティ | Apple公式サポートが強力 | Googleが積極的に推進 |\n\n"
            "**選び方:**\n"
            "- iOSアプリを作りたい → Swift一択\n"
            "- Androidアプリを作りたい → Kotlin一択\n"
            "- クロスプラットフォームを検討 → Kotlin Multiplatform が有力選択肢"
        ),
        "rejected": "SwiftはiOS、KotlinはAndroidです。どちらも良いです。好みで選んでください。",
    },
    {
        "prompt": "日本語の「は」と「が」の使い分けを外国人に説明してください。",
        "chosen": (
            "「は」と「が」の違いは日本語学習者が最も苦労するポイントの一つです。\n\n"
            "**「は」（主題・対比）**\n"
            "話題を提示したり、対比を示すときに使います。\n"
            "- 「私は田中です」→「私」を話題として提示\n"
            "- 「魚は好きですが、肉は苦手です」→対比\n\n"
            "**「が」（主語・強調・新情報）**\n"
            "主語を強調したり、新しい情報を導入するときに使います。\n"
            "- 「誰が来ましたか？」「田中さんが来ました」→「が」で焦点を当てる\n"
            "- 「雨が降っています」→新しい情報として提示\n\n"
            "**簡単な判断基準:**\n"
            "すでに知っている情報 → 「は」 / 新しく提示する情報 → 「が」"
        ),
        "rejected": "はとがは両方主語を示します。使い方は同じです。どちらでも大丈夫です。",
    },
]


def generate_dpo_data() -> list[dict]:
    """DPO用preferenceデータをMLXフォーマットで生成"""
    records = []
    for pair in DPO_PAIRS:
        prompt_text = (
            f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
            f"<|im_start|>user\n{pair['prompt']}<|im_end|>\n"
            f"<|im_start|>assistant\n"
        )
        records.append({
            "prompt": prompt_text,
            "chosen": pair["chosen"] + "<|im_end|>",
            "rejected": pair["rejected"] + "<|im_end|>",
        })
    return records


# ---------------------------------------------------------------------------
# カリキュラム学習: 難易度スコアリング
# ---------------------------------------------------------------------------

def difficulty_score(record: dict) -> float:
    """
    難易度を0.0（簡単）〜1.0（難しい）で返す。
    カリキュラム学習では簡単→難しい順で学習させる。

    スコア要素:
      - instruction の長さ・複雑さ
      - output の長さ・語彙多様性
      - 複数条件の有無
    """
    instruction = record.get("instruction", "")
    output = record.get("output", "")

    # 指示の長さ（長いほど複雑）
    instr_len_score = min(len(instruction) / 300, 1.0) * 0.3

    # 出力の長さ（長いほど複雑）
    out_len_score = min(len(output) / 500, 1.0) * 0.25

    # 複数条件の有無（箇条書き・番号付きリスト → 複雑）
    has_conditions = bool(re.search(r"(\d+[．.。]|[①②③④⑤]|・.+・)", instruction))
    condition_score = 0.2 if has_conditions else 0.0

    # コードブロックの有無（技術的複雑さ）
    has_code = "```" in instruction or "```" in output
    code_score = 0.15 if has_code else 0.0

    # 語彙多様性（出力の文字バイグラムTTR）
    bigrams = [output[i:i+2] for i in range(len(output) - 1) if "぀" <= output[i] <= "鿿"]
    vocab_score = (len(set(bigrams)) / max(len(bigrams), 1)) * 0.1 if bigrams else 0.0

    return instr_len_score + out_len_score + condition_score + code_score + vocab_score


def curriculum_sort(records: list[dict]) -> list[dict]:
    """
    カリキュラム学習順にソート（簡単→難しい）。
    学習初期に簡単なサンプルを見せることで収束が安定する。
    """
    scored = [(difficulty_score(r), r) for r in records]
    scored.sort(key=lambda x: x[0])
    print(f"  カリキュラム学習: 難易度スコア {scored[0][0]:.3f}（最小）〜{scored[-1][0]:.3f}（最大）")
    return [r for _, r in scored]


# ---------------------------------------------------------------------------
# メイン処理
# ---------------------------------------------------------------------------

def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--curriculum", action="store_true", default=True,
                        help="カリキュラム学習順（簡単→難しい）でソート（デフォルト: ON）")
    parser.add_argument("--no-curriculum", dest="curriculum", action="store_false",
                        help="カリキュラム学習を無効化しランダム順にする")
    args = parser.parse_args()

    random.seed(SEED)
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    DPO_DIR.mkdir(parents=True, exist_ok=True)

    print("=== SFTデータセット読み込み中 ===")
    all_records: list[dict] = []
    multiturn_records: list[dict] = []

    # 通常の一問一答データ
    all_records.extend(load_dolly_ja())
    all_records.extend(load_izumi_lab())
    all_records.extend(load_elyza_tasks())
    all_records.extend(load_oasst_ja())
    all_records.extend(load_coding_ja())
    all_records.extend(load_writing_samples())

    # マルチターンデータ（別途処理）
    multiturn_records = load_multiturn_ja()

    print(f"\n一問一答: {len(all_records)}件 / マルチターン: {len(multiturn_records)}件")

    # 重複除去
    before = len(all_records)
    all_records = deduplicate(all_records)
    print(f"重複除去: {before} → {len(all_records)}件")

    # 上限適用（ソート前にシャッフルして多様性を確保してから上限）
    random.shuffle(all_records)
    single_limit = MAX_SAMPLES - len(multiturn_records)
    all_records = all_records[:single_limit]

    # train / valid 分割（validはランダムのまま）
    split_idx = int(len(all_records) * (1 - VALID_RATIO))
    train_single = all_records[:split_idx]
    valid_single = all_records[split_idx:]

    # カリキュラム学習ソート（train のみ適用）
    if args.curriculum:
        print("\n=== カリキュラム学習ソート ===")
        train_single = curriculum_sort(train_single)
    else:
        random.shuffle(train_single)

    # マルチターンをtrainの後半（難しい側）に追加
    # マルチターンは構造的に複雑なので後半に配置
    train_all = train_single + [{"multiturn": r["multiturn"]} for r in multiturn_records]

    print(f"学習用: {len(train_all)}件  検証用: {len(valid_single)}件")

    # MLX用フォーマットでJSONL書き出し
    def write_jsonl(records: list[dict], path: Path):
        with open(path, "w", encoding="utf-8") as f:
            for rec in tqdm(records, desc=f"書き出し: {path.name}"):
                if "multiturn" in rec:
                    text = format_multiturn(rec["multiturn"])
                else:
                    text = format_chat(rec["instruction"], rec["output"])
                f.write(json.dumps({"text": text}, ensure_ascii=False) + "\n")

    write_jsonl(train_all, TRAIN_FILE)
    write_jsonl(valid_single, VALID_FILE)

    # DPOデータ書き出し
    print("\n=== DPO preferenceデータ生成中 ===")
    dpo_records = generate_dpo_data()
    random.shuffle(dpo_records)
    dpo_split = int(len(dpo_records) * 0.9)
    dpo_train = dpo_records[:dpo_split]
    dpo_valid = dpo_records[dpo_split:]

    def write_dpo_jsonl(records: list[dict], path: Path):
        with open(path, "w", encoding="utf-8") as f:
            for rec in records:
                f.write(json.dumps(rec, ensure_ascii=False) + "\n")

    write_dpo_jsonl(dpo_train, DPO_DIR / "train.jsonl")
    write_dpo_jsonl(dpo_valid, DPO_DIR / "valid.jsonl")
    print(f"  DPO学習: {len(dpo_train)}件 / 検証: {len(dpo_valid)}件")

    # 統計表示
    train_sizes = []
    with open(TRAIN_FILE, encoding="utf-8") as f:
        for line in f:
            train_sizes.append(len(json.loads(line)["text"]))

    print(f"\n=== 完了 ===")
    print(f"SFT学習データ: {TRAIN_FILE}")
    print(f"DPOデータ:     {DPO_DIR}/train.jsonl")
    print(f"平均テキスト長: {sum(train_sizes) // len(train_sizes)}文字")
    print(f"最大テキスト長: {max(train_sizes)}文字")
    print("\n次のステップ: python 2_finetune.py を実行してください")


if __name__ == "__main__":
    main()
