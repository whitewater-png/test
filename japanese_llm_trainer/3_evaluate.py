#!/usr/bin/env python3
"""
日本語能力評価スクリプト（拡張版）
ファインチューニング前後のモデルを比較評価する

追加評価軸:
    6. 敬語適切性       - 場面に応じた敬語レベルの使い分け
    7. 文体一貫性       - です・ます調 / だ・である調の混在検出
    8. 語彙品質         - 語彙の豊富さと反復の少なさ

使い方:
    python 3_evaluate.py [--model-path ./models/qwen3-8b-japanese]
    python 3_evaluate.py --compare
    python 3_evaluate.py --model-path ./models/qwen3-8b-japanese-dpo --compare
"""

import json
import re
import subprocess
import sys
import time
from pathlib import Path
from dataclasses import dataclass, field


# ---------------------------------------------------------------------------
# 評価タスク定義
# ---------------------------------------------------------------------------

@dataclass
class EvalTask:
    id: str
    category: str
    prompt: str
    reference: str
    check_keywords: list[str] = field(default_factory=list)
    avoid_keywords: list[str] = field(default_factory=list)
    expected_style: str = ""   # "formal" | "casual" | "mixed_ok" | ""
    max_tokens: int = 300


EVAL_TASKS: list[EvalTask] = [
    # ---- 文法正確性 ----
    EvalTask(
        id="grammar_01",
        category="文法正確性",
        prompt="「ご利用になられる」という表現の問題点を指摘して、正しい表現を教えてください。",
        reference="二重敬語の指摘と「ご利用になる」または「利用される」への修正",
        check_keywords=["二重敬語", "ご利用になる"],
        expected_style="formal",
    ),
    EvalTask(
        id="grammar_02",
        category="文法正確性",
        prompt="「全然大丈夫です」は正しい日本語ですか？根拠も含めて説明してください。",
        reference="本来は否定表現と使うが現代では肯定的文脈でも許容される",
        check_keywords=["否定", "現代"],
        expected_style="formal",
    ),
    EvalTask(
        id="grammar_03",
        category="文法正確性",
        prompt="次の文を適切な敬語に変換してください：「明日、田中さんに会う予定があります。」",
        reference="明日、田中様にお会いする予定でございます",
        check_keywords=["田中様", "お会い"],
        expected_style="formal",
    ),
    EvalTask(
        id="grammar_04",
        category="文法正確性",
        prompt="「〜させていただきます」という表現を過剰に使うことの問題点を教えてください。",
        reference="許可不要な場面での使用は謙遜過剰・不自然に聞こえる",
        check_keywords=["過剰", "不自然"],
        expected_style="formal",
    ),

    # ---- 語彙豊富性 ----
    EvalTask(
        id="vocab_01",
        category="語彙豊富性",
        prompt="「嬉しい」という感情を表す日本語の表現を5つ教えてください。それぞれのニュアンスの違いも説明してください。",
        reference="喜ぶ・有頂天・欣喜雀躍・晴れやか・うれしはずむなど",
        check_keywords=["ニュアンス"],
    ),
    EvalTask(
        id="vocab_02",
        category="語彙豊富性",
        prompt="「頑張る」の代わりに使える、より具体的で印象的な動詞を3つ提案し、例文も示してください。",
        reference="励む・邁進する・精進するなど具体的な代替語と例文",
        check_keywords=["励む"],
    ),
    EvalTask(
        id="vocab_03",
        category="語彙豊富性",
        prompt="「美しい」を使わずに夕焼けの美しさを表現する文を3つ書いてください。",
        reference="情景を豊かな語彙で表現した3文",
        check_keywords=["夕"],
        avoid_keywords=["美しい"],
    ),

    # ---- コーディング ----
    EvalTask(
        id="coding_01",
        category="コーディング",
        prompt=(
            "以下のPythonコードのバグを見つけて修正し、日本語で説明してください。\n\n"
            "```python\n"
            "def calculate_average(numbers):\n"
            "    total = 0\n"
            "    for n in numbers:\n"
            "        total += n\n"
            "    return total / len(numbers)\n\n"
            "print(calculate_average([]))\n"
            "```"
        ),
        reference="空リストのZeroDivisionErrorを指摘し条件分岐またはtry-exceptで修正",
        check_keywords=["ZeroDivisionError", "空"],
        max_tokens=400,
    ),
    EvalTask(
        id="coding_02",
        category="コーディング",
        prompt="Pythonで辞書のリストを特定のキーでソートする方法を、コード例付きで日本語で説明してください。",
        reference="sorted()とlambdaまたはoperator.itemgetterを使った例",
        check_keywords=["sorted", "lambda"],
        max_tokens=400,
    ),
    EvalTask(
        id="coding_03",
        category="コーディング",
        prompt="GitのrebaseとmergeはどのVに違うか、図や例を交えて日本語で説明してください。",
        reference="コミット履歴の整合性、rebaseは線形履歴、mergeはマージコミット",
        check_keywords=["コミット", "履歴"],
        max_tokens=500,
    ),
    EvalTask(
        id="coding_04",
        category="コーディング",
        prompt="Pythonのasync/awaitを使った非同期処理を初心者向けに説明してください。",
        reference="asyncio、コルーチン、イベントループの基本説明とコード例",
        check_keywords=["async", "await"],
        max_tokens=500,
    ),

    # ---- 文章執筆 ----
    EvalTask(
        id="writing_01",
        category="文章執筆",
        prompt="「締め切りに間に合わなかったことをお詫びするビジネスメール」を書いてください。",
        reference="謝罪・原因・再発防止・今後の対応を含む丁寧なビジネスメール",
        check_keywords=["申し訳", "対応"],
        expected_style="formal",
        max_tokens=400,
    ),
    EvalTask(
        id="writing_02",
        category="文章執筆",
        prompt="新しいカフェのオープン告知SNS投稿文を、ハッシュタグ付きで書いてください。",
        reference="魅力的なコピー文とハッシュタグを含む投稿",
        check_keywords=["#"],
        expected_style="casual",
        max_tokens=200,
    ),
    EvalTask(
        id="writing_03",
        category="文章執筆",
        prompt="「継続は力なり」という諺を使って、200字程度の短文を書いてください。",
        reference="諺を自然に組み込んだ200字程度の文章",
        check_keywords=["継続は力なり"],
        max_tokens=300,
    ),
    EvalTask(
        id="writing_04",
        category="文章執筆",
        prompt="技術ブログの書き出し文を、読者の興味を引くように書いてください。テーマは「Pythonで業務を自動化した話」です。",
        reference="読者を引き込む冒頭文、具体的な問題提起を含む",
        check_keywords=["Python", "自動"],
        max_tokens=200,
    ),

    # ---- 指示理解 ----
    EvalTask(
        id="instruction_01",
        category="指示理解",
        prompt=(
            "以下の条件を全て満たす文章を作成してください：\n"
            "1. 50字以内\n"
            "2. 「桜」という言葉を含む\n"
            "3. 疑問文にする\n"
            "4. 春の情景を描写する"
        ),
        reference="50字以内・桜を含む・疑問文・春の情景",
        check_keywords=["桜", "？"],
        max_tokens=100,
    ),
    EvalTask(
        id="instruction_02",
        category="指示理解",
        prompt=(
            "次の文章を3つの異なるスタイルで言い換えてください：\n"
            "原文：「この製品は品質が良い」\n"
            "スタイル：①フォーマル ②カジュアル ③詩的"
        ),
        reference="3つの異なるスタイルの言い換え",
        check_keywords=["フォーマル", "カジュアル", "詩的"],
        max_tokens=200,
    ),
    EvalTask(
        id="instruction_03",
        category="指示理解",
        prompt=(
            "以下の文章を要約してください（3点箇条書きで）：\n\n"
            "機械学習は、データから自動的にパターンを学習するAI技術です。"
            "従来のプログラミングでは人間がルールを明示的に定義しますが、"
            "機械学習ではアルゴリズムがデータから規則性を見つけ出します。"
            "代表的な手法には、教師あり学習、教師なし学習、強化学習があります。"
        ),
        reference="3点箇条書きで機械学習の特徴を要約",
        check_keywords=["・", "学習"],
        max_tokens=200,
    ),

    # ---- 敬語適切性（新規） ----
    EvalTask(
        id="keigo_01",
        category="敬語適切性",
        prompt="取引先の担当者に初めてメールを送る際の書き出し文を書いてください。",
        reference="初めてご連絡申し上げます等の適切な書き出し",
        check_keywords=["はじめ", "申し上げ"],
        expected_style="formal",
        max_tokens=150,
    ),
    EvalTask(
        id="keigo_02",
        category="敬語適切性",
        prompt="上司から「明日の会議は中止になりました」と言われたとき、メールで返信する文を書いてください。",
        reference="承知いたしました・了解いたしました等の適切な返答",
        check_keywords=["承知", "いたし"],
        expected_style="formal",
        max_tokens=150,
    ),
    EvalTask(
        id="keigo_03",
        category="敬語適切性",
        prompt="友人グループのLINEで「明日ランチどうする？」という状況の返信を書いてください。",
        reference="カジュアルな口調での返信",
        check_keywords=["！", "笑"],
        expected_style="casual",
        avoid_keywords=["いたします", "申し上げ", "ございます"],
        max_tokens=100,
    ),
    EvalTask(
        id="keigo_04",
        category="敬語適切性",
        prompt="会議で「何かご質問はありますか？」と聞かれ、質問があるときの答え方を教えてください。",
        reference="「〜についてお伺いしたいのですが」等の適切な質問表現",
        check_keywords=["お伺い", "よろしい"],
        expected_style="formal",
        max_tokens=200,
    ),

    # ---- 文体一貫性（新規） ----
    EvalTask(
        id="style_01",
        category="文体一貫性",
        prompt="「テレワークのメリットとデメリット」について、です・ます調で説明してください。",
        reference="です・ます調で一貫して書かれた文章",
        check_keywords=["です", "ます"],
        avoid_keywords=["である", "だ。"],
        expected_style="formal",
        max_tokens=300,
    ),
    EvalTask(
        id="style_02",
        category="文体一貫性",
        prompt="個人ブログ向けに「最近ハマっているコーヒーの話」をカジュアルな文体で書いてください。",
        reference="カジュアルな文体で一貫して書かれた文章",
        check_keywords=["コーヒー"],
        avoid_keywords=["いたします", "申し上げ"],
        expected_style="casual",
        max_tokens=300,
    ),
]


# ---------------------------------------------------------------------------
# 日本語品質チェック関数
# ---------------------------------------------------------------------------

def check_style_consistency(text: str) -> dict:
    """です・ます調とだ・である調の混在を検出"""
    desu_masu = len(re.findall(r"(です|ます|ました|ません|ますか|ますね)", text))
    da_de_aru = len(re.findall(r"(である\b|だ。|した。(?!が)|する。|ない。)", text))
    total = desu_masu + da_de_aru

    if total == 0:
        consistency = 1.0
        style = "neutral"
    elif desu_masu == 0:
        consistency = 1.0
        style = "da_de_aru"
    elif da_de_aru == 0:
        consistency = 1.0
        style = "desu_masu"
    else:
        majority = max(desu_masu, da_de_aru)
        consistency = majority / total
        style = "mixed"

    return {
        "desu_masu_count": desu_masu,
        "da_de_aru_count": da_de_aru,
        "consistency": round(consistency, 3),
        "style": style,
        "is_consistent": consistency >= 0.85,
    }


def check_keigo_level(text: str) -> dict:
    """敬語レベルを判定"""
    sonkei = len(re.findall(r"(いらっしゃ|おっしゃ|なさ|くださ|ご覧|お見え)", text))
    kenjou = len(re.findall(r"(いたし|申し|拝|存じ|参り|伺|おり)", text))
    teichou = len(re.findall(r"(です|ます|ございます|でしょう)", text))
    casual = len(re.findall(r"(だよ|だね|じゃん|っていう|してる|してた|やばい)", text))

    if sonkei + kenjou >= 3:
        level = "very_formal"
    elif teichou >= 5 and casual == 0:
        level = "formal"
    elif casual >= 3:
        level = "casual"
    else:
        level = "neutral"

    return {
        "sonkei_count": sonkei,
        "kenjou_count": kenjou,
        "teichou_count": teichou,
        "casual_count": casual,
        "level": level,
    }


def check_vocabulary_richness(text: str) -> dict:
    """語彙の豊富さを測定"""
    # 日本語の単語（文字レベルで簡易計算）
    chars = [c for c in text if "぀" <= c <= "鿿" or "a" <= c <= "z" or "A" <= c <= "Z"]
    if not chars:
        return {"type_token_ratio": 0, "richness_score": 0}

    # バイグラムでType-Token Ratio計算
    bigrams = [text[i:i+2] for i in range(len(text) - 1) if "぀" <= text[i] <= "鿿"]
    if not bigrams:
        return {"type_token_ratio": 0, "richness_score": 0}

    ttr = len(set(bigrams)) / len(bigrams)

    # 反復フレーズペナルティ
    repetition_penalty = 0
    if re.search(r"(.{5,})\1", text):
        repetition_penalty = 0.2

    richness = max(0, ttr - repetition_penalty)
    return {
        "type_token_ratio": round(ttr, 3),
        "richness_score": round(richness, 3),
        "has_repetition": repetition_penalty > 0,
    }


def check_style_match(task: EvalTask, response: str) -> dict:
    """タスクの期待スタイルと実際のスタイルが合っているか"""
    if not task.expected_style:
        return {"match": True, "score": 1.0}

    keigo = check_keigo_level(response)
    actual = keigo["level"]

    if task.expected_style == "formal":
        match = actual in ("very_formal", "formal")
        score = 1.0 if actual == "very_formal" else (0.8 if actual == "formal" else 0.3)
    elif task.expected_style == "casual":
        match = actual == "casual"
        score = 1.0 if match else (0.5 if actual == "neutral" else 0.2)
    else:
        match = True
        score = 1.0

    return {"match": match, "score": round(score, 2), "actual_level": actual}


# ---------------------------------------------------------------------------
# 推論エンジン
# ---------------------------------------------------------------------------

SYSTEM_PROMPT = (
    "あなたは優秀な日本語AIアシスタントです。"
    "アプリケーション開発と文章執筆を得意とし、"
    "常に正確で自然な日本語で回答します。"
)


def run_mlx_inference(
    model_path: str,
    prompt: str,
    adapter_path: str | None,
    max_tokens: int,
) -> str:
    full_prompt = (
        f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
        f"<|im_start|>user\n{prompt}<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )

    cmd = [
        sys.executable, "-m", "mlx_lm.generate",
        "--model", model_path,
        "--max-tokens", str(max_tokens),
        "--temp", "0.3",
        "--prompt", full_prompt,
    ]

    if adapter_path:
        cmd += ["--adapter-path", adapter_path]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=False)
        output = result.stdout.strip()
        if "<|im_start|>assistant" in output:
            output = output.split("<|im_start|>assistant")[-1].strip()
        if "<|im_end|>" in output:
            output = output.split("<|im_end|>")[0].strip()
        return output or "(出力なし)"
    except subprocess.TimeoutExpired:
        return "(タイムアウト)"
    except Exception as e:
        return f"(エラー: {e})"


# ---------------------------------------------------------------------------
# 統合スコアリング
# ---------------------------------------------------------------------------

def score_response(task: EvalTask, response: str) -> dict:
    """キーワード・スタイル・品質を統合したスコアリング"""
    # キーワードチェック
    keyword_score = 0
    keyword_hits = []
    for kw in task.check_keywords:
        if kw in response:
            keyword_score += 10
            keyword_hits.append(kw)

    avoid_penalty = 0
    avoid_hits = []
    for kw in task.avoid_keywords:
        if kw in response:
            avoid_penalty += 15
            avoid_hits.append(kw)

    # 長さスコア
    length = len(response)
    if length < 20:
        length_score = 0
    elif length < 50:
        length_score = 15
    else:
        length_score = 30

    # 日本語比率スコア
    ja_chars = sum(1 for c in response if "぀" <= c <= "鿿")
    ja_ratio = ja_chars / max(len(response), 1)
    ja_score = int(ja_ratio * 20)

    # 文体一貫性スコア（最大10点）
    style_info = check_style_consistency(response)
    consistency_score = int(style_info["consistency"] * 10)

    # スタイルマッチスコア（最大10点）
    style_match = check_style_match(task, response)
    style_match_score = int(style_match["score"] * 10)

    # 語彙豊富性スコア（最大10点）
    vocab_info = check_vocabulary_richness(response)
    vocab_score = int(vocab_info["richness_score"] * 10)

    total = max(0, (
        keyword_score
        + length_score
        + ja_score
        + consistency_score
        + style_match_score
        + vocab_score
        - avoid_penalty
    ))
    max_possible = max(1, len(task.check_keywords)) * 10 + 30 + 20 + 10 + 10 + 10

    return {
        "keyword_score": keyword_score,
        "keyword_hits": keyword_hits,
        "avoid_hits": avoid_hits,
        "length_score": length_score,
        "ja_score": ja_score,
        "consistency_score": consistency_score,
        "style_match_score": style_match_score,
        "vocab_score": vocab_score,
        "total": total,
        "max_possible": max_possible,
        "percentage": round(total / max_possible * 100, 1),
        "style_info": style_info,
        "vocab_info": vocab_info,
        "style_match": style_match,
    }


# ---------------------------------------------------------------------------
# 評価ループ
# ---------------------------------------------------------------------------

def evaluate_model(
    model_path: str,
    adapter_path: str | None = None,
    output_file: str = "eval_results/results.json",
) -> dict:
    print(f"\n評価対象: {model_path}")
    if adapter_path:
        print(f"アダプター: {adapter_path}")

    Path(output_file).parent.mkdir(parents=True, exist_ok=True)

    results = []
    category_scores: dict[str, list[float]] = {}

    for i, task in enumerate(EVAL_TASKS, 1):
        print(f"\n[{i:02d}/{len(EVAL_TASKS)}] {task.category} - {task.id}")
        print(f"  プロンプト: {task.prompt[:60]}...")

        start = time.time()
        response = run_mlx_inference(model_path, task.prompt, adapter_path, task.max_tokens)
        elapsed = time.time() - start

        scores = score_response(task, response)
        style_label = scores["style_info"]["style"]
        print(f"  スコア: {scores['percentage']:.1f}%  文体: {style_label}  ({elapsed:.1f}秒)")
        print(f"  応答: {response[:80]}...")

        results.append({
            "task_id": task.id,
            "category": task.category,
            "prompt": task.prompt,
            "response": response,
            "reference": task.reference,
            "scores": scores,
            "elapsed_sec": round(elapsed, 2),
        })

        cat = task.category
        if cat not in category_scores:
            category_scores[cat] = []
        category_scores[cat].append(scores["percentage"])

    all_pcts = [r["scores"]["percentage"] for r in results]
    overall_avg = sum(all_pcts) / len(all_pcts)

    # 文体一貫性の集計
    consistency_values = [
        r["scores"]["style_info"]["consistency"]
        for r in results
        if r["scores"]["style_info"]["style"] != "neutral"
    ]
    avg_consistency = (
        sum(consistency_values) / len(consistency_values)
        if consistency_values else 1.0
    )

    summary = {
        "model_path": model_path,
        "adapter_path": adapter_path,
        "overall_average": round(overall_avg, 1),
        "avg_style_consistency": round(avg_consistency * 100, 1),
        "category_averages": {
            cat: round(sum(scores) / len(scores), 1)
            for cat, scores in category_scores.items()
        },
        "total_tasks": len(results),
        "results": results,
    }

    with open(output_file, "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    return summary


def print_report(summary: dict, label: str = "評価レポート"):
    print("\n" + "=" * 60)
    print(label)
    print("=" * 60)
    print(f"総合スコア:       {summary['overall_average']:.1f}%")
    print(f"文体一貫性スコア: {summary.get('avg_style_consistency', 'N/A')}%")
    print()
    print("カテゴリ別スコア:")
    for cat, avg in summary["category_averages"].items():
        bar = "█" * int(avg / 5) + "░" * (20 - int(avg / 5))
        print(f"  {cat:<12} {bar} {avg:.1f}%")


def print_comparison(base: dict, ft: dict):
    print("\n" + "=" * 60)
    print("比較レポート: ベース vs ファインチューニング済み")
    print("=" * 60)

    diff = ft["overall_average"] - base["overall_average"]
    sign = "+" if diff >= 0 else ""
    print(f"総合スコア: {base['overall_average']:.1f}% → {ft['overall_average']:.1f}%  ({sign}{diff:.1f}%)")

    base_con = base.get("avg_style_consistency", 0)
    ft_con = ft.get("avg_style_consistency", 0)
    con_diff = ft_con - base_con
    sign_c = "+" if con_diff >= 0 else ""
    print(f"文体一貫性: {base_con:.1f}% → {ft_con:.1f}%  ({sign_c}{con_diff:.1f}%)")

    print()
    print("カテゴリ別変化:")
    all_cats = set(base["category_averages"]) | set(ft["category_averages"])
    for cat in sorted(all_cats):
        b = base["category_averages"].get(cat, 0)
        f = ft["category_averages"].get(cat, 0)
        d = f - b
        arrow = "↑" if d > 1 else ("↓" if d < -1 else "→")
        sign_d = "+" if d >= 0 else ""
        print(f"  {cat:<12} {b:.1f}% {arrow} {f:.1f}%  ({sign_d}{d:.1f}%)")


# ---------------------------------------------------------------------------
# エントリーポイント
# ---------------------------------------------------------------------------

def main():
    import argparse

    parser = argparse.ArgumentParser(description="日本語LLM評価スクリプト（拡張版）")
    parser.add_argument("--model-path", default="./models/qwen3-8b-japanese")
    parser.add_argument("--base-model-path", default="./models/qwen3-8b")
    parser.add_argument("--use-adapter", action="store_true")
    parser.add_argument("--adapter-path", default="./adapters/japanese-dpo")
    parser.add_argument("--compare", action="store_true", help="ベースモデルとの比較")
    parser.add_argument("--output-dir", default="./eval_results")
    args = parser.parse_args()

    Path(args.output_dir).mkdir(parents=True, exist_ok=True)

    if args.compare:
        print("\n[1/2] ベースモデルを評価中...")
        base = evaluate_model(
            model_path=args.base_model_path,
            adapter_path=None,
            output_file=f"{args.output_dir}/base_results.json",
        )

        print("\n[2/2] ファインチューニング済みモデルを評価中...")
        ft_model = args.model_path if not args.use_adapter else args.base_model_path
        ft_adapter = args.adapter_path if args.use_adapter else None
        ft = evaluate_model(
            model_path=ft_model,
            adapter_path=ft_adapter,
            output_file=f"{args.output_dir}/finetuned_results.json",
        )

        print_comparison(base, ft)

        comparison = {
            "base": base,
            "finetuned": ft,
            "improvement": {
                "overall": round(ft["overall_average"] - base["overall_average"], 1),
                "style_consistency": round(
                    ft.get("avg_style_consistency", 0) - base.get("avg_style_consistency", 0), 1
                ),
                "by_category": {
                    cat: round(
                        ft["category_averages"].get(cat, 0) - base["category_averages"].get(cat, 0), 1
                    )
                    for cat in base["category_averages"]
                },
            },
        }
        comp_file = f"{args.output_dir}/comparison.json"
        with open(comp_file, "w", encoding="utf-8") as f:
            json.dump(comparison, f, ensure_ascii=False, indent=2)
        print(f"\n比較結果保存: {comp_file}")

    else:
        adapter = args.adapter_path if args.use_adapter else None
        summary = evaluate_model(
            model_path=args.model_path,
            adapter_path=adapter,
            output_file=f"{args.output_dir}/results.json",
        )
        print_report(summary)


if __name__ == "__main__":
    main()
