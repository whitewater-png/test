#!/usr/bin/env python3
"""
日本語能力評価スクリプト
ファインチューニング前後のモデルを比較評価する

使い方:
    pip install mlx-lm ollama
    python 3_evaluate.py [--model-path ./models/qwen3-8b-japanese] [--use-adapter]

評価カテゴリ:
    1. 文法正確性       - 助詞・敬語の正しい使用
    2. 語彙豊富性       - 適切な語彙の選択
    3. コーディング     - 日本語でのコード説明・生成
    4. 文章執筆         - 自然な文章構成
    5. 指示理解         - 複雑な指示の正確な実行
"""

import json
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
    reference: str        # 参考解答（自動スコアリングの基準）
    check_keywords: list[str] = field(default_factory=list)  # 含まれるべきキーワード
    avoid_keywords: list[str] = field(default_factory=list)  # 含まれてはいけないキーワード
    max_tokens: int = 300


EVAL_TASKS: list[EvalTask] = [
    # ---- 文法正確性 ----
    EvalTask(
        id="grammar_01",
        category="文法正確性",
        prompt="「ご利用になられる」という表現の問題点を指摘して、正しい表現を教えてください。",
        reference="二重敬語の指摘と「ご利用になる」または「利用される」への修正",
        check_keywords=["二重敬語", "ご利用になる"],
        avoid_keywords=[],
    ),
    EvalTask(
        id="grammar_02",
        category="文法正確性",
        prompt="「全然大丈夫です」は正しい日本語ですか？",
        reference="本来否定表現と使うが現代では肯定的文脈でも許容される",
        check_keywords=["否定", "現代"],
    ),
    EvalTask(
        id="grammar_03",
        category="文法正確性",
        prompt="次の文を敬語に変換してください：「明日、田中さんに会う予定があります。」",
        reference="明日、田中様にお会いする予定でございます",
        check_keywords=["田中様", "お会い"],
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
        prompt="「頑張る」の代わりに使える、より具体的で印象的な動詞を3つ提案してください。",
        reference="励む・邁進する・精進するなど具体的な代替語",
        check_keywords=["励む"],
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
        reference="空リストのZeroDivisionErrorを指摘し、条件分岐またはtry-exceptで修正",
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
        prompt="GitのrebaseとmergeはどのV違うか、図や例を交えて日本語で説明してください。",
        reference="コミット履歴の整合性、rebaseは線形履歴、mergeはマージコミットの違い",
        check_keywords=["コミット", "履歴"],
        max_tokens=500,
    ),

    # ---- 文章執筆 ----
    EvalTask(
        id="writing_01",
        category="文章執筆",
        prompt="「締め切りに間に合わなかったことをお詫びするビジネスメール」を書いてください。",
        reference="謝罪・原因・再発防止・今後の対応を含む丁寧なビジネスメール",
        check_keywords=["申し訳", "対応"],
        max_tokens=400,
    ),
    EvalTask(
        id="writing_02",
        category="文章執筆",
        prompt="新しいカフェのオープン告知SNS投稿文を、ハッシュタグ付きで書いてください。",
        reference="魅力的なコピー文とハッシュタグを含む投稿",
        check_keywords=["#"],
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
]


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
    """MLXを使って推論を実行"""
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
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        output = result.stdout.strip()
        # MLXの出力からプロンプト部分を除去
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
# 自動スコアリング
# ---------------------------------------------------------------------------

def score_response(task: EvalTask, response: str) -> dict:
    """ルールベースの自動スコアリング"""
    scores = {}

    # キーワードチェック（各キーワード = 10点）
    keyword_score = 0
    keyword_hits = []
    for kw in task.check_keywords:
        if kw in response:
            keyword_score += 10
            keyword_hits.append(kw)

    # 禁止キーワードチェック（各ヒット = -20点）
    avoid_penalty = 0
    avoid_hits = []
    for kw in task.avoid_keywords:
        if kw in response:
            avoid_penalty += 20
            avoid_hits.append(kw)

    # 長さスコア（出力なし・極端に短い場合はペナルティ）
    length = len(response)
    if length < 20:
        length_score = 0
    elif length < 50:
        length_score = 20
    else:
        length_score = 40

    # 日本語文字の割合スコア
    ja_chars = sum(1 for c in response if "぀" <= c <= "鿿")
    ja_ratio = ja_chars / max(len(response), 1)
    ja_score = int(ja_ratio * 20)  # 最大20点

    total = max(0, keyword_score + length_score + ja_score - avoid_penalty)
    max_possible = max(1, len(task.check_keywords)) * 10 + 60  # キーワード + 長さ + 日本語

    scores = {
        "keyword_score": keyword_score,
        "keyword_hits": keyword_hits,
        "avoid_hits": avoid_hits,
        "length_score": length_score,
        "ja_score": ja_score,
        "total": total,
        "max_possible": max_possible,
        "percentage": round(total / max_possible * 100, 1),
    }
    return scores


# ---------------------------------------------------------------------------
# メイン評価ループ
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
        response = run_mlx_inference(
            model_path=model_path,
            prompt=task.prompt,
            adapter_path=adapter_path,
            max_tokens=task.max_tokens,
        )
        elapsed = time.time() - start

        scores = score_response(task, response)
        print(f"  スコア: {scores['percentage']:.1f}% ({elapsed:.1f}秒)")
        print(f"  応答: {response[:80]}...")

        result = {
            "task_id": task.id,
            "category": task.category,
            "prompt": task.prompt,
            "response": response,
            "reference": task.reference,
            "scores": scores,
            "elapsed_sec": round(elapsed, 2),
        }
        results.append(result)

        cat = task.category
        if cat not in category_scores:
            category_scores[cat] = []
        category_scores[cat].append(scores["percentage"])

    # 集計
    all_pcts = [r["scores"]["percentage"] for r in results]
    overall_avg = sum(all_pcts) / len(all_pcts)

    summary = {
        "model_path": model_path,
        "adapter_path": adapter_path,
        "overall_average": round(overall_avg, 1),
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


def print_report(summary: dict):
    print("\n" + "=" * 60)
    print("評価レポート")
    print("=" * 60)
    print(f"総合スコア: {summary['overall_average']:.1f}%")
    print()
    print("カテゴリ別スコア:")
    for cat, avg in summary["category_averages"].items():
        bar = "█" * int(avg / 5) + "░" * (20 - int(avg / 5))
        print(f"  {cat:<12} {bar} {avg:.1f}%")
    print()
    print(f"詳細結果: {summary.get('output_file', 'eval_results/results.json')}")


# ---------------------------------------------------------------------------
# エントリーポイント
# ---------------------------------------------------------------------------

def main():
    import argparse

    parser = argparse.ArgumentParser(description="日本語LLM評価スクリプト")
    parser.add_argument(
        "--model-path",
        default="./models/qwen3-8b-japanese",
        help="評価するモデルのパス（デフォルト: ファインチューニング済みモデル）",
    )
    parser.add_argument(
        "--base-model-path",
        default="./models/qwen3-8b",
        help="比較用ベースモデルのパス",
    )
    parser.add_argument(
        "--use-adapter",
        action="store_true",
        help="マージ済みモデルの代わりにアダプターを使用",
    )
    parser.add_argument(
        "--adapter-path",
        default="./adapters/japanese-v1",
        help="LoRAアダプターのパス（--use-adapter 時に使用）",
    )
    parser.add_argument(
        "--compare",
        action="store_true",
        help="ベースモデルとファインチューニング済みモデルを比較",
    )
    parser.add_argument(
        "--output-dir",
        default="./eval_results",
        help="評価結果の出力ディレクトリ",
    )
    args = parser.parse_args()

    Path(args.output_dir).mkdir(parents=True, exist_ok=True)

    if args.compare:
        # ベースモデルの評価
        print("\n[1/2] ベースモデルを評価中...")
        base_summary = evaluate_model(
            model_path=args.base_model_path,
            adapter_path=None,
            output_file=f"{args.output_dir}/base_results.json",
        )
        base_summary["output_file"] = f"{args.output_dir}/base_results.json"

        # ファインチューニング済みモデルの評価
        print("\n[2/2] ファインチューニング済みモデルを評価中...")
        ft_summary = evaluate_model(
            model_path=args.model_path if not args.use_adapter else args.base_model_path,
            adapter_path=args.adapter_path if args.use_adapter else None,
            output_file=f"{args.output_dir}/finetuned_results.json",
        )
        ft_summary["output_file"] = f"{args.output_dir}/finetuned_results.json"

        # 比較レポート
        print("\n" + "=" * 60)
        print("比較レポート: ベース vs ファインチューニング済み")
        print("=" * 60)
        print(f"総合スコア: {base_summary['overall_average']:.1f}% → {ft_summary['overall_average']:.1f}%  ", end="")
        diff = ft_summary["overall_average"] - base_summary["overall_average"]
        print(f"({'+'if diff >= 0 else ''}{diff:.1f}%)")
        print()
        print("カテゴリ別変化:")
        for cat in base_summary["category_averages"]:
            b = base_summary["category_averages"].get(cat, 0)
            f = ft_summary["category_averages"].get(cat, 0)
            d = f - b
            arrow = "↑" if d > 0 else ("↓" if d < 0 else "→")
            print(f"  {cat:<12} {b:.1f}% {arrow} {f:.1f}%  ({'+' if d >= 0 else ''}{d:.1f}%)")

        # 比較結果をJSON保存
        comparison = {
            "base": base_summary,
            "finetuned": ft_summary,
            "improvement": {
                "overall": round(diff, 1),
                "by_category": {
                    cat: round(ft_summary["category_averages"].get(cat, 0) - base_summary["category_averages"].get(cat, 0), 1)
                    for cat in base_summary["category_averages"]
                },
            },
        }
        comp_file = f"{args.output_dir}/comparison.json"
        with open(comp_file, "w", encoding="utf-8") as f:
            json.dump(comparison, f, ensure_ascii=False, indent=2)
        print(f"\n比較結果保存: {comp_file}")

    else:
        # 単体評価
        adapter = args.adapter_path if args.use_adapter else None
        summary = evaluate_model(
            model_path=args.model_path,
            adapter_path=adapter,
            output_file=f"{args.output_dir}/results.json",
        )
        summary["output_file"] = f"{args.output_dir}/results.json"
        print_report(summary)


if __name__ == "__main__":
    main()
