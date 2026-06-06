#!/usr/bin/env python3
"""
SPIN (Self-Play Fine-Tuning) 反復自己改善ループ
参考論文: Self-Play Fine-Tuning Converts Weak Language Models to Strong (2024)

仕組み:
  各イテレーションで:
    1. 現在のモデルが複数の回答候補を生成
    2. ローカルLLM-Judgeが全候補を採点（APIなし）
    3. 高スコア → chosen / 低スコア → rejected のペア自動生成
    4. ORPOで差分学習（現イテレーションのモデルを更新）
    5. 改善幅が閾値未満になれば収束・終了

使い方:
    python 4_spin.py [--config config_14b.yaml] [--iterations 5] [--test-run]
    python 4_spin.py --from-iteration 2   # 途中再開

M4 Max 36GB メモリ推移:
    生成フェーズ: ~9GB  (推論のみ)
    採点フェーズ: ~9GB  (Judgeモデル推論)
    学習フェーズ: ~21GB (ORPO)
    ※ 各フェーズは順番に実行されるため同時にはかからない
"""

import argparse
import json
import random
import re
import subprocess
import sys
import time
import yaml
from pathlib import Path


# ---------------------------------------------------------------------------
# 設定
# ---------------------------------------------------------------------------

DEFAULT_CONFIG = {
    "model_path": "./models/qwen3-14b-japanese-orpo",  # ORPO済みモデルから開始
    "base_model_path": "./models/qwen3-14b",
    "spin_data_dir": "./data/spin",
    "spin_adapter_base": "./adapters/spin",             # iter1 → spin-iter1/
    "spin_model_base": "./models/qwen3-14b-spin",       # iter1 → qwen3-14b-spin-iter1/
    "max_iterations": 5,
    "convergence_threshold": 2.0,   # Judge スコア改善が2点未満で収束
    "candidates_per_prompt": 4,     # 各プロンプトで生成する候補数
    "generation_temp": 0.8,         # 多様性のため高め
    "judge_temp": 0.1,
    "orpo_iters": 300,              # SPINは少ないステップでよい
    "orpo_batch_size": 2,
    "orpo_lr": 5e-5,                # 精調整なので小さめ
    "orpo_beta": 0.1,
    "lora_layers": 8,
    "max_seq_length": 1024,
    "grad_checkpoint": True,
    "seed": 42,
}

SYSTEM_PROMPT = (
    "あなたは優秀な日本語AIアシスタントです。"
    "アプリケーション開発と文章執筆を得意とし、"
    "常に正確で自然な日本語で回答します。"
)

JUDGE_SYSTEM = "あなたは日本語の専門家です。回答を評価してJSON形式のみで返してください。"

JUDGE_TEMPLATE = """以下の日本語回答を評価してください。

【質問】{question}
【回答】{answer}

JSON形式のみで返答:
{{"naturalness":0,"accuracy":0,"keigo":0,"vocabulary":0,"overall":0}}

各項目は1〜10の整数。overallが総合評価。"""

# ---------------------------------------------------------------------------
# SPINプロンプトセット（多様なカテゴリをカバー）
# ---------------------------------------------------------------------------

SPIN_PROMPTS = [
    # ビジネス文書
    "新規プロジェクトの提案書の書き出し文を、説得力のある形で書いてください。",
    "退職者への送別メッセージを、温かみのある文体で書いてください。",
    "クレームメールへの謝罪と対応策を含む返信を書いてください。",
    "社内向けシステム障害の報告メールを書いてください。",
    "取引先への年末挨拶メールを書いてください。",
    # 敬語・文体
    "「資料を送ってください」を最も丁寧な敬語に変換してください。",
    "上司への「会議に遅れます」という連絡を敬語で書いてください。",
    "「確認しました」をビジネスメール向けの表現に変換してください。",
    "「わかりません」を顧客対応で使える丁寧な表現にしてください。",
    "「後で連絡します」を取引先向けの丁寧な表現にしてください。",
    # コーディング説明（日本語）
    "Pythonの`with`文の役割と使いどころを初心者向けに説明してください。",
    "SQLのINNER JOINとLEFT JOINの違いをわかりやすく説明してください。",
    "Gitのコンフリクトが起きたとき、どう解決するか手順を説明してください。",
    "REST APIとGraphQLの違いを、メリット・デメリットを交えて説明してください。",
    "Dockerコンテナを使うメリットを、具体例を交えて説明してください。",
    # 文章執筆
    "「時間の使い方」をテーマに、ビジネスパーソン向けの短いコラムを書いてください。",
    "新しいコーヒーショップのオープン告知を、SNS向けに書いてください。",
    "「失敗から学ぶ」というテーマで、200字程度の短文を書いてください。",
    "技術書のまえがきとして自然な文章を書いてください。",
    "ユーザーインタビューの謝辞メールを書いてください。",
    # 複合指示
    "以下の3点を全て含む文章を書いてください: ①Pythonに言及、②です・ます調、③100字以内",
    "「継続は力なり」を使って、エンジニア向けのモチベーション文を書いてください。",
    "次の文を①フォーマル②カジュアルの2スタイルで言い換えてください: 「バグを直しました」",
    "会議の議事録の冒頭部分（日時・参加者・目的）を書いてください。",
    "製品レビューの返信を、感謝と改善への約束を含めて書いてください。",
    # 語彙・表現
    "「頑張る」を使わずに、努力を表す文を3通り書いてください。",
    "「とても良かった」を5種類の表現に言い換えてください。",
    "「すぐに」の類義語を3つ挙げ、ニュアンスの違いを説明してください。",
    "「問題が発生しました」をビジネス文書向けの表現に変えてください。",
    "「考えてみます」の丁寧な言い換えを3つ教えてください。",
]


# ---------------------------------------------------------------------------
# ユーティリティ
# ---------------------------------------------------------------------------

def mlx_generate(model_path: str, prompt: str, adapter: str | None,
                 max_tokens: int, temp: float) -> str:
    full = (
        f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
        f"<|im_start|>user\n{prompt}<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )
    cmd = [
        sys.executable, "-m", "mlx_lm.generate",
        "--model", model_path,
        "--max-tokens", str(max_tokens),
        "--temp", str(temp),
        "--prompt", full,
    ]
    if adapter:
        cmd += ["--adapter-path", adapter]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=False)
        out = r.stdout.strip()
        if "<|im_start|>assistant" in out:
            out = out.split("<|im_start|>assistant")[-1].strip()
        if "<|im_end|>" in out:
            out = out.split("<|im_end|>")[0].strip()
        return out or ""
    except Exception:
        return ""


def mlx_judge(judge_model: str, question: str, answer: str) -> dict | None:
    if not answer:
        return None
    prompt = JUDGE_TEMPLATE.format(question=question, answer=answer)
    full = (
        f"<|im_start|>system\n{JUDGE_SYSTEM}<|im_end|>\n"
        f"<|im_start|>user\n{prompt}<|im_end|>\n"
        f"<|im_start|>assistant\n"
    )
    cmd = [
        sys.executable, "-m", "mlx_lm.generate",
        "--model", judge_model,
        "--max-tokens", "80",
        "--temp", "0.1",
        "--prompt", full,
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, check=False)
        raw = r.stdout.strip()
        m = re.search(r"\{[^{}]+\}", raw)
        if not m:
            return None
        scores = json.loads(m.group())
        required = ["naturalness", "accuracy", "keigo", "vocabulary", "overall"]
        if all(isinstance(scores.get(k), (int, float)) and 1 <= scores[k] <= 10 for k in required):
            return scores
        return None
    except Exception:
        return None


def run_orpo_iter(model_path: str, adapter_path: str | None, data_dir: str,
                  out_adapter: str, config: dict, test_run: bool = False) -> bool:
    """1イテレーションのORPO学習"""
    Path(out_adapter).mkdir(parents=True, exist_ok=True)
    iters = 10 if test_run else config["orpo_iters"]

    # ORPO はベースモデル + アダプター or マージ済みモデルを使用
    cmd = [
        sys.executable, "-m", "mlx_lm.lora",
        "--model", model_path,
        "--train",
        "--loss", "orpo",
        "--data", data_dir,
        "--adapter-path", out_adapter,
        "--iters", str(iters),
        "--batch-size", str(config["orpo_batch_size"]),
        "--learning-rate", str(config["orpo_lr"]),
        "--lora-layers", str(config["lora_layers"]),
        "--max-seq-length", str(config["max_seq_length"]),
        "--beta", str(config["orpo_beta"]),
        "--seed", str(config["seed"]),
    ]
    if adapter_path:
        cmd += ["--adapter-path-start", adapter_path]
    if config.get("grad_checkpoint"):
        cmd.append("--grad-checkpoint")

    result = subprocess.run(cmd, check=False)
    return result.returncode == 0


def fuse_adapter(model_path: str, adapter_path: str, out_path: str):
    Path(out_path).mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, "-m", "mlx_lm.fuse",
        "--model", model_path,
        "--adapter-path", adapter_path,
        "--save-path", out_path,
    ]
    subprocess.run(cmd, check=False)


# ---------------------------------------------------------------------------
# SPINの各フェーズ
# ---------------------------------------------------------------------------

def phase_generate(model_path: str, adapter: str | None,
                   prompts: list[str], n_candidates: int,
                   temp: float, test_run: bool) -> dict[str, list[str]]:
    """各プロンプトに対してn_candidates個の回答を生成"""
    use_prompts = prompts[:5] if test_run else prompts
    results: dict[str, list[str]] = {}
    total = len(use_prompts) * n_candidates
    done = 0

    print(f"  生成: {len(use_prompts)}プロンプト × {n_candidates}候補 = {total}件")
    for prompt in use_prompts:
        candidates = []
        for _ in range(n_candidates):
            resp = mlx_generate(model_path, prompt, adapter, max_tokens=300, temp=temp)
            if resp:
                candidates.append(resp)
            done += 1
            print(f"  [{done:3d}/{total}] 生成中...", end="\r")
        results[prompt] = candidates

    print(f"  生成完了: {sum(len(v) for v in results.values())}件")
    return results


def phase_judge(judge_model: str, candidates: dict[str, list[str]]) -> dict[str, list[dict]]:
    """全候補をJudgeで採点"""
    scored: dict[str, list[dict]] = {}
    total = sum(len(v) for v in candidates.values())
    done = 0

    print(f"  採点: {total}件")
    for prompt, responses in candidates.items():
        prompt_scores = []
        for resp in responses:
            score = mlx_judge(judge_model, prompt, resp)
            prompt_scores.append({
                "response": resp,
                "scores": score,
                "overall": score["overall"] if score else 0,
            })
            done += 1
            print(f"  [{done:3d}/{total}] 採点中...", end="\r")
        scored[prompt] = prompt_scores

    print(f"  採点完了")
    return scored


def phase_create_pairs(scored: dict[str, list[dict]],
                       min_score_gap: float = 2.0) -> list[dict]:
    """高スコアをchosen、低スコアをrejectedとしてペアを生成"""
    pairs = []
    skipped = 0

    for prompt, items in scored.items():
        valid = [x for x in items if x["scores"] is not None]
        if len(valid) < 2:
            skipped += 1
            continue

        valid.sort(key=lambda x: x["overall"], reverse=True)
        best = valid[0]
        worst = valid[-1]

        # スコア差が閾値以上のペアのみ採用（差が小さいと学習シグナルが弱い）
        if best["overall"] - worst["overall"] < min_score_gap:
            skipped += 1
            continue

        prompt_text = (
            f"<|im_start|>system\n{SYSTEM_PROMPT}<|im_end|>\n"
            f"<|im_start|>user\n{prompt}<|im_end|>\n"
            f"<|im_start|>assistant\n"
        )
        pairs.append({
            "prompt": prompt_text,
            "chosen": best["response"] + "<|im_end|>",
            "rejected": worst["response"] + "<|im_end|>",
            "score_gap": best["overall"] - worst["overall"],
            "chosen_score": best["overall"],
            "rejected_score": worst["overall"],
        })

    print(f"  ペア生成: {len(pairs)}件（スキップ: {skipped}件）")
    return pairs


def compute_avg_score(scored: dict[str, list[dict]]) -> float:
    """採点済みデータから平均スコアを計算"""
    all_scores = [
        item["overall"]
        for items in scored.values()
        for item in items
        if item["scores"] is not None
    ]
    return sum(all_scores) / len(all_scores) if all_scores else 0.0


def save_state(state_file: Path, state: dict):
    with open(state_file, "w", encoding="utf-8") as f:
        json.dump(state, f, ensure_ascii=False, indent=2)


def load_state(state_file: Path) -> dict:
    if state_file.exists():
        with open(state_file, encoding="utf-8") as f:
            return json.load(f)
    return {"iteration": 0, "scores": [], "current_model": None, "current_adapter": None}


# ---------------------------------------------------------------------------
# メインSPINループ
# ---------------------------------------------------------------------------

def spin_loop(config: dict, test_run: bool = False, start_iter: int = 1):
    random.seed(config["seed"])

    state_file = Path(config["spin_data_dir"]) / "spin_state.json"
    state = load_state(state_file)

    # 開始モデルの決定
    if start_iter == 1 and state["iteration"] == 0:
        current_model = config["model_path"]
        current_adapter = None
    elif start_iter > 1 or state["iteration"] > 0:
        # 途中再開: 前回のイテレーション結果を使用
        prev_iter = max(start_iter - 1, state["iteration"])
        prev_model = f"{config['spin_model_base']}-iter{prev_iter}"
        if Path(prev_model).exists():
            current_model = prev_model
            current_adapter = None
        else:
            current_model = config["model_path"]
            current_adapter = f"{config['spin_adapter_base']}-iter{prev_iter}"
        print(f"再開: iter {prev_iter + 1} から (モデル: {current_model})")
    else:
        current_model = config["model_path"]
        current_adapter = None

    max_iters = 2 if test_run else config["max_iterations"]
    prev_avg_score = state["scores"][-1] if state["scores"] else 0.0

    print(f"\n開始モデル: {current_model}")
    print(f"最大イテレーション: {max_iters}")
    print(f"収束閾値: Judge改善 < {config['convergence_threshold']}点")
    print(f"プロンプト数: {len(SPIN_PROMPTS)}（test_run時: 5）")

    for iteration in range(start_iter, start_iter + max_iters):
        print(f"\n{'='*60}")
        print(f"SPIN Iteration {iteration}/{start_iter + max_iters - 1}")
        print(f"{'='*60}")

        spin_iter_dir = Path(config["spin_data_dir"]) / f"iter{iteration}"
        spin_iter_dir.mkdir(parents=True, exist_ok=True)

        out_adapter = f"{config['spin_adapter_base']}-iter{iteration}"
        out_model = f"{config['spin_model_base']}-iter{iteration}"

        iter_start = time.time()

        # Phase 1: 生成
        print(f"\n[Phase 1/4] 回答候補の生成")
        n_cand = 2 if test_run else config["candidates_per_prompt"]
        candidates = phase_generate(
            model_path=current_model,
            adapter=current_adapter,
            prompts=SPIN_PROMPTS,
            n_candidates=n_cand,
            temp=config["generation_temp"],
            test_run=test_run,
        )

        # Phase 2: Judge採点
        print(f"\n[Phase 2/4] LLM-as-Judge 採点")
        judge_model = config.get("judge_model_path") or config["base_model_path"]
        scored = phase_judge(judge_model=judge_model, candidates=candidates)

        current_avg = compute_avg_score(scored)
        improvement = current_avg - prev_avg_score
        print(f"  平均スコア: {prev_avg_score:.2f} → {current_avg:.2f} ({'+' if improvement >= 0 else ''}{improvement:.2f})")

        # Phase 3: Preferenceペア生成
        print(f"\n[Phase 3/4] Preferenceペア生成")
        pairs = phase_create_pairs(scored, min_score_gap=config["convergence_threshold"])

        if not pairs:
            print("  ペアが生成できませんでした（スコア差が小さい → 収束）")
            break

        # JSONL保存
        train_file = spin_iter_dir / "train.jsonl"
        valid_file = spin_iter_dir / "valid.jsonl"
        split = max(1, len(pairs) // 10)
        train_pairs, valid_pairs = pairs[split:], pairs[:split]

        for path, data in [(train_file, train_pairs), (valid_file, valid_pairs)]:
            with open(path, "w", encoding="utf-8") as f:
                for pair in data:
                    f.write(json.dumps({
                        "prompt": pair["prompt"],
                        "chosen": pair["chosen"],
                        "rejected": pair["rejected"],
                    }, ensure_ascii=False) + "\n")

        print(f"  学習ペア: {len(train_pairs)}件 / 検証: {len(valid_pairs)}件")

        # Phase 4: ORPO学習
        print(f"\n[Phase 4/4] ORPO 差分学習 ({config['orpo_iters']}ステップ)")
        print(f"  メモリ: ~21GB（学習中）")
        success = run_orpo_iter(
            model_path=current_model,
            adapter_path=current_adapter,
            data_dir=str(spin_iter_dir),
            out_adapter=out_adapter,
            config=config,
            test_run=test_run,
        )

        if not success:
            print(f"  ORPO学習失敗。イテレーション{iteration}をスキップします")
            break

        # マージ
        fuse_adapter(current_model, out_adapter, out_model)

        elapsed = (time.time() - iter_start) / 60
        state["iteration"] = iteration
        state["scores"].append(current_avg)
        state["current_model"] = out_model
        state["current_adapter"] = out_adapter
        save_state(state_file, state)

        print(f"\n  Iteration {iteration} 完了 ({elapsed:.0f}分)")
        print(f"  スコア: {current_avg:.2f}/10")
        print(f"  新モデル: {out_model}")

        # 収束判定
        if iteration > start_iter and improvement < config["convergence_threshold"]:
            print(f"\n収束検出: 改善幅 {improvement:.2f} < 閾値 {config['convergence_threshold']}")
            print("SPINループを終了します")
            break

        # 次イテレーションの準備
        current_model = out_model
        current_adapter = None
        prev_avg_score = current_avg

    # 最終レポート
    print(f"\n{'='*60}")
    print("SPIN 完了レポート")
    print(f"{'='*60}")
    print(f"総イテレーション: {len(state['scores'])}")
    if state["scores"]:
        print(f"スコア推移: {' → '.join(f'{s:.2f}' for s in state['scores'])}")
        total_gain = state["scores"][-1] - (state["scores"][0] if len(state["scores"]) > 1 else prev_avg_score)
        print(f"総改善幅: {total_gain:+.2f}/10")
    print(f"最終モデル: {state['current_model']}")
    print(f"\n評価コマンド:")
    print(f"  python 3_evaluate.py --model-path {state['current_model']} --compare --use-llm-judge")

    return state


# ---------------------------------------------------------------------------
# エントリーポイント
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="SPIN 反復自己改善ループ")
    parser.add_argument("--config", default="config_14b.yaml")
    parser.add_argument("--iterations", type=int, default=None, help="最大イテレーション数（config上書き）")
    parser.add_argument("--test-run", action="store_true", help="テスト実行（各フェーズを短縮）")
    parser.add_argument("--from-iteration", type=int, default=1, help="指定イテレーションから再開")
    parser.add_argument("--judge-model", default=None, help="Judgeに使うモデルパス（省略時はbase_model_path）")
    args = parser.parse_args()

    print("=" * 60)
    print("SPIN 反復自己改善ループ")
    print("APIなし・ローカルLLMによる自律的品質向上")
    print("=" * 60)

    # 設定読み込み
    config = DEFAULT_CONFIG.copy()
    if Path(args.config).exists():
        with open(args.config, encoding="utf-8") as f:
            raw = yaml.safe_load(f)
        for k in ["model_path", "base_model_path", "lora_layers", "batch_size",
                  "max_seq_length", "grad_checkpoint", "seed"]:
            if k in raw:
                config[k] = raw[k]
        if "spin" in raw:
            config.update(raw["spin"])

    if args.iterations:
        config["max_iterations"] = args.iterations
    if args.judge_model:
        config["judge_model_path"] = args.judge_model

    # 前提チェック
    if not Path(config["model_path"]).exists():
        print(f"エラー: モデルが見つかりません: {config['model_path']}")
        print("→ 先に run_pipeline.sh --model 14b を実行してください")
        sys.exit(1)

    Path(config["spin_data_dir"]).mkdir(parents=True, exist_ok=True)

    print(f"\n設定:")
    print(f"  ベースモデル:     {config['model_path']}")
    print(f"  最大イテレーション: {config['max_iterations']}")
    print(f"  プロンプト数:     {len(SPIN_PROMPTS)}")
    print(f"  候補数/プロンプト: {config['candidates_per_prompt']}")
    print(f"  ORPOステップ数:   {config['orpo_iters']}")
    print(f"\nメモリ使用量の推移:")
    print(f"  生成フェーズ: ~9GB  → 他アプリに27GB")
    print(f"  採点フェーズ: ~9GB  → 他アプリに27GB")
    print(f"  学習フェーズ: ~21GB → 他アプリに15GB")

    if args.test_run:
        print("\n[テストモード] 各フェーズを短縮実行します")

    spin_loop(config, test_run=args.test_run, start_iter=args.from_iteration)


if __name__ == "__main__":
    main()
