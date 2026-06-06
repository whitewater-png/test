#!/usr/bin/env python3
"""
モデルマージスクリプト（TIES / DARE / SLERP / Linear）

複数のファインチューニング済みモデルの重みを融合し、
それぞれの長所を1つのモデルに統合する。学習不要・数分で完了。

代表的な用途:
  - SPINの各イテレーションのチェックポイントを融合（過学習を平均化）
  - 自作の日本語モデル + ELYZA-JP など他の日本語特化モデルを融合
  - ORPOモデル + SPINモデルを融合

マージ手法:
  linear : 単純な重み付き平均（最も安全）
  slerp  : 球面線形補間（2モデル専用・滑らかな融合）
  ties   : 符号の競合を解決して融合（3モデル以上に強い）
  dare   : ランダムに重みを間引いてから融合（過学習抑制）

使い方:
  # 設定ファイルから
  python 5_merge.py --recipe merge_recipe.yaml

  # SPINチェックポイントを自動収集して融合
  python 5_merge.py --auto-spin --method ties

  バックエンドに mergekit を使用（pip install mergekit）。
  mergekit は HuggingFace 形式で動作するため、MLXモデルを事前にHF形式へ
  変換する必要がある場合がある（--from-mlx で自動変換）。
"""

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml


def check_mergekit() -> bool:
    try:
        import mergekit  # noqa: F401
        return True
    except ImportError:
        return False


def install_hint():
    print("mergekit がインストールされていません。")
    print("  pip install mergekit")
    print("  （Apple Silicon でも動作します。マージはCPU/メモリのみ使用）")


# ---------------------------------------------------------------------------
# マージレシピ生成
# ---------------------------------------------------------------------------

def build_recipe(models: list[dict], method: str, base_model: str | None) -> dict:
    """
    mergekit のYAMLレシピを生成する。
    models: [{"path": "...", "weight": 0.5, "density": 0.5}, ...]
    """
    method = method.lower()

    if method == "slerp":
        if len(models) != 2:
            raise ValueError("slerp は2モデル専用です")
        return {
            "slerp": True,
            "merge_method": "slerp",
            "base_model": models[0]["path"],
            "models": [{"model": models[1]["path"]}],
            "parameters": {"t": models[1].get("weight", 0.5)},
            "dtype": "bfloat16",
        }

    if method == "linear":
        return {
            "merge_method": "linear",
            "models": [
                {"model": m["path"], "parameters": {"weight": m.get("weight", 1.0 / len(models))}}
                for m in models
            ],
            "dtype": "bfloat16",
        }

    if method in ("ties", "dare", "dare_ties"):
        mk_method = "dare_ties" if method == "dare" else method
        if not base_model:
            base_model = models[0]["path"]
        return {
            "merge_method": mk_method,
            "base_model": base_model,
            "models": [
                {
                    "model": m["path"],
                    "parameters": {
                        "weight": m.get("weight", 0.5),
                        "density": m.get("density", 0.5),  # DARE/TIESの保持率
                    },
                }
                for m in models
            ],
            "dtype": "bfloat16",
        }

    raise ValueError(f"未知のマージ手法: {method}")


def auto_collect_spin(spin_state_file: str) -> list[dict]:
    """SPINの各イテレーションモデルを収集"""
    state_path = Path(spin_state_file)
    if not state_path.exists():
        print(f"SPIN状態ファイルが見つかりません: {spin_state_file}")
        return []

    with open(state_path, encoding="utf-8") as f:
        state = json.load(f)

    base = state.get("current_model", "").rsplit("-iter", 1)[0]
    n_iters = state.get("iteration", 0)

    models = []
    # 後半のイテレーションほど高い重み（より洗練されている前提）
    for i in range(1, n_iters + 1):
        model_path = f"{base}-iter{i}"
        if Path(model_path).exists():
            weight = i / sum(range(1, n_iters + 1))  # 線形増加の重み
            models.append({"path": model_path, "weight": round(weight, 3), "density": 0.6})

    print(f"SPINチェックポイント収集: {len(models)}個")
    for m in models:
        print(f"  {m['path']} (weight={m['weight']})")
    return models


# ---------------------------------------------------------------------------
# マージ実行
# ---------------------------------------------------------------------------

def run_merge(recipe: dict, output_path: str):
    """mergekit-yaml を呼んでマージを実行"""
    Path(output_path).mkdir(parents=True, exist_ok=True)

    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False, encoding="utf-8") as f:
        yaml.safe_dump(recipe, f, allow_unicode=True)
        recipe_file = f.name

    print(f"\nマージレシピ:\n{yaml.safe_dump(recipe, allow_unicode=True)}")

    cmd = [
        "mergekit-yaml",
        recipe_file,
        output_path,
        "--allow-crimes",       # 同一アーキ間のマージを許可
        "--out-shard-size", "5B",
        "--lazy-unpickle",      # メモリ節約（M4 Max向け）
    ]
    print("実行:", " ".join(cmd))
    result = subprocess.run(cmd, check=False)

    Path(recipe_file).unlink(missing_ok=True)

    if result.returncode == 0:
        print(f"\nマージ完了: {output_path}")
        return True
    else:
        print(f"\nマージ失敗 (終了コード: {result.returncode})")
        return False


def convert_to_mlx(hf_path: str, mlx_path: str, quantize: bool = True):
    """マージ後のHFモデルをMLX形式に変換（量子化付き）"""
    print(f"\nMLX形式へ変換中: {mlx_path}")
    cmd = [
        sys.executable, "-m", "mlx_lm.convert",
        "--hf-path", hf_path,
        "--mlx-path", mlx_path,
    ]
    if quantize:
        cmd += ["--quantize", "--q-bits", "4"]
    subprocess.run(cmd, check=False)


# ---------------------------------------------------------------------------
# エントリーポイント
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="モデルマージ（TIES/DARE/SLERP/Linear）")
    parser.add_argument("--recipe", help="マージレシピYAMLファイル")
    parser.add_argument("--method", default="ties", choices=["linear", "slerp", "ties", "dare", "dare_ties"])
    parser.add_argument("--auto-spin", action="store_true", help="SPINチェックポイントを自動収集して融合")
    parser.add_argument("--spin-state", default="./data/spin/spin_state.json")
    parser.add_argument("--base-model", default="./models/qwen3-14b", help="TIES/DARE用のベースモデル")
    parser.add_argument("--output", default="./models/qwen3-14b-merged")
    parser.add_argument("--to-mlx", action="store_true", help="マージ後にMLX形式へ変換")
    parser.add_argument("--mlx-output", default="./models/qwen3-14b-merged-mlx")
    args = parser.parse_args()

    print("=" * 60)
    print("モデルマージ")
    print("=" * 60)

    if not check_mergekit():
        install_hint()
        sys.exit(1)

    # レシピの決定
    if args.recipe:
        with open(args.recipe, encoding="utf-8") as f:
            recipe = yaml.safe_load(f)
        print(f"レシピ読み込み: {args.recipe}")
    elif args.auto_spin:
        models = auto_collect_spin(args.spin_state)
        if len(models) < 2:
            print("マージには2個以上のモデルが必要です。SPINを複数回実行してください。")
            sys.exit(1)
        recipe = build_recipe(models, args.method, args.base_model)
    else:
        print("--recipe または --auto-spin を指定してください")
        print("\n例（手動レシピ）: merge_recipe.yaml を作成して --recipe で指定")
        print("例（SPIN自動）:   python 5_merge.py --auto-spin --method ties")
        sys.exit(1)

    # マージ実行
    success = run_merge(recipe, args.output)
    if not success:
        sys.exit(1)

    # MLX変換
    if args.to_mlx:
        convert_to_mlx(args.output, args.mlx_output, quantize=True)
        print(f"\n評価コマンド:")
        print(f"  python 3_evaluate.py --model-path {args.mlx_output} --compare --use-llm-judge")
    else:
        print(f"\nMLX形式で使うには:")
        print(f"  python 5_merge.py --recipe ... --to-mlx")


if __name__ == "__main__":
    main()
